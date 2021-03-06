// Copyright 2017 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#include "pik.h"

#include <string>
#include <vector>

#undef PROFILER_ENABLED
#define PROFILER_ENABLED 1
#include "adaptive_quantization.h"
#include "common.h"
#include "compressed_image.h"
#include "headers.h"
#include "image.h"
#include "multipass_handler.h"
#include "noise.h"
#include "os_specific.h"
#include "pik_multipass.h"
#include "pik_params.h"
#include "pik_pass.h"
#include "profiler.h"
#include "quantizer.h"
#include "saliency_map.h"
#include "single_image_handler.h"

namespace pik {

namespace {
static const uint8_t kBrunsliMagic[] = {
  0x0A, 0x04, 'B', 0xd2, 0xd5, 'N', 0x12
};

// TODO(user): use VerifySignature, when brunsli codebase is attached.
bool IsBrunsliFile(const PaddedBytes& compressed) {
  const size_t magic_size = sizeof(kBrunsliMagic);
  if (compressed.size() < magic_size) {
    return false;
  }
  if (memcmp(compressed.data(), kBrunsliMagic, magic_size) != 0) {
    return false;
  }
  return true;
}

Status BrunsliToPixels(const DecompressParams& dparams,
                       const PaddedBytes& compressed, CodecInOut* io,
                       PikInfo* aux_out, ThreadPool* pool) {
  return PIK_FAILURE("Brunsli decoding is not implemented yet.");
}

}  // namespace

Status PixelsToPik(const CompressParams& cparams, const CodecInOut* io,
                   PaddedBytes* compressed, PikInfo* aux_out,
                   ThreadPool* pool) {
  if (io->xsize() == 0 || io->ysize() == 0) {
    return PIK_FAILURE("Empty image");
  }
  if (!io->HasOriginalBitsPerSample()) {
    return PIK_FAILURE(
        "Pik requires specifying original bit depth "
        "of the pixels to encode as metadata.");
  }
  FileHeader container;
  MakeFileHeader(cparams, io, &container);

  if (!cparams.lossless_base.empty()) {
    SingleImageManager transform;
    PikMultipassEncoder encoder(container, compressed, &transform, aux_out);
    CompressParams p = cparams;

    if (ApplyOverride(cparams.adaptive_reconstruction,
                      cparams.butteraugli_distance >=
                          kMinButteraugliForAdaptiveReconstruction)) {
      transform.UseAdaptiveReconstruction();
    }

    // Lossless base.
    CodecInOut base_io(io->Context());
    PIK_RETURN_IF_ERROR(base_io.SetFromFile(cparams.lossless_base, pool));
    p.adaptive_reconstruction = Override::kOff;
    p.lossless_mode = true;
    PIK_RETURN_IF_ERROR(
        encoder.AddPass(p, PassParams{/*is_last=*/false}, &base_io, pool));

    // Final non-lossless pass.
    p.adaptive_reconstruction = cparams.adaptive_reconstruction;
    p.lossless_mode = false;
    PIK_RETURN_IF_ERROR(
        encoder.AddPass(p, PassParams{/*is_last=*/true}, io, pool));
    PIK_RETURN_IF_ERROR(encoder.Finalize());
    return true;
  }

  if (!cparams.progressive_mode) {
    size_t extension_bits, total_bits;
    PIK_CHECK(CanEncode(container, &extension_bits, &total_bits));

    compressed->resize(DivCeil(total_bits, kBitsPerByte));
    size_t pos = 0;
    PIK_RETURN_IF_ERROR(
        WriteFileHeader(container, extension_bits, &pos, compressed->data()));
    PassParams pass_params;
    pass_params.is_last = true;
    SingleImageManager transform;
    PIK_RETURN_IF_ERROR(PixelsToPikPass(cparams, pass_params, io, pool,
                                        compressed, pos, aux_out, &transform));
  } else {
    bool lossless = cparams.lossless_mode;
    SingleImageManager transform;
    PikMultipassEncoder encoder(container, compressed, &transform, aux_out);
    CompressParams p = cparams;
    PassParams pass_params;
    p.lossless_mode = false;

    if (ApplyOverride(cparams.adaptive_reconstruction,
                      cparams.butteraugli_distance >=
                          kMinButteraugliForAdaptiveReconstruction)) {
      transform.UseAdaptiveReconstruction();
    }

    // Disable adaptive reconstruction in intermediate passes.
    p.adaptive_reconstruction = Override::kOff;
    pass_params.is_last = false;

    // DC + Low frequency pass.
    transform.SetProgressiveMode(ProgressiveMode::kLfOnly);
    PIK_RETURN_IF_ERROR(encoder.AddPass(p, pass_params, io, pool));

    // Disable gradient map from here on.
    p.gradient = Override::kOff;

    // DC + LF are 0, predictions are useless.
    p.predict_lf = false;
    p.predict_hf = false;

    // Optional salient-regions high frequency pass.
    auto final_pass_progressive_mode = ProgressiveMode::kHfOnly;
    if (!cparams.saliency_extractor_for_progressive_mode.empty()) {
      std::shared_ptr<ImageF> saliency_map;
      PIK_RETURN_IF_ERROR(
          ProduceSaliencyMap(cparams, compressed, io, pool, &saliency_map));
      final_pass_progressive_mode = ProgressiveMode::kNonSalientHfOnly;
      transform.SetProgressiveMode(ProgressiveMode::kSalientHfOnly);
      transform.SetSaliencyMap(saliency_map);
      PIK_RETURN_IF_ERROR(encoder.AddPass(p, pass_params, io, pool));
    }

    // Final non-lossless pass.
    transform.SetProgressiveMode(final_pass_progressive_mode);
    p.adaptive_reconstruction = cparams.adaptive_reconstruction;
    if (!lossless) {
      pass_params.is_last = true;
    }
    PIK_RETURN_IF_ERROR(encoder.AddPass(p, pass_params, io, pool));
    if (lossless) {
      pass_params.is_last = true;
      p.lossless_mode = true;
      PIK_RETURN_IF_ERROR(encoder.AddPass(p, pass_params, io, pool));
    }
    PIK_RETURN_IF_ERROR(encoder.Finalize());
  }
  return true;
}

Status PikToPixels(const DecompressParams& dparams,
                   const PaddedBytes& compressed, CodecInOut* io,
                   PikInfo* aux_out, ThreadPool* pool) {
  PROFILER_ZONE("PikToPixels uninstrumented");

  if (IsBrunsliFile(compressed)) {
    return BrunsliToPixels(dparams, compressed, io, aux_out, pool);
  }

  // To avoid the complexity of file I/O and buffering, we assume the bitstream
  // is loaded (or for large images/sequences: mapped into) memory.
  BitReader reader(compressed.data(), compressed.size());
  FileHeader container;
  PIK_RETURN_IF_ERROR(ReadFileHeader(&reader, &container));

  // Preview is discardable, i.e. content image does not rely on decoded preview
  // pixels; just skip it, if any.
  size_t preview_size_bits = container.preview.size_bits;
  if (preview_size_bits != 0) {
    reader.SkipBits(preview_size_bits);
  }

  SingleImageManager transform;
  do {
    PIK_RETURN_IF_ERROR(PikPassToPixels(dparams, compressed, container, pool,
                                        &reader, io, aux_out, &transform));
  } while (!transform.IsLastPass());

  if (dparams.check_decompressed_size &&
      reader.Position() != compressed.size()) {
    return PIK_FAILURE("Pik compressed data size mismatch.");
  }

  io->enc_size = compressed.size();

  return true;
}

}  // namespace pik
