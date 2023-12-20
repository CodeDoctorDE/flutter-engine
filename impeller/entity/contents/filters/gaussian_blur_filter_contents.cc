// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/gaussian_blur_filter_contents.h"

#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/texture_fill.frag.h"
#include "impeller/entity/texture_fill.vert.h"
#include "impeller/renderer/command.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/sampler_library.h"
#include "impeller/renderer/vertex_buffer_builder.h"

namespace impeller {

using GaussianBlurVertexShader = GaussianBlurPipeline::VertexShader;
using GaussianBlurFragmentShader = GaussianBlurPipeline::FragmentShader;

namespace {

std::optional<Rect> ExpandCoverageHint(const std::optional<Rect>& coverage_hint,
                                       const Matrix& source_to_local_transform,
                                       const Vector2& padding) {
  if (!coverage_hint.has_value()) {
    return std::nullopt;
  }
  Vector2 transformed_padding = (source_to_local_transform * padding).Abs();
  return coverage_hint->Expand(transformed_padding);
}

SamplerDescriptor MakeSamplerDescriptor(MinMagFilter filter,
                                        SamplerAddressMode address_mode) {
  SamplerDescriptor sampler_desc;
  sampler_desc.min_filter = filter;
  sampler_desc.mag_filter = filter;
  sampler_desc.width_address_mode = address_mode;
  sampler_desc.height_address_mode = address_mode;
  return sampler_desc;
}

template <typename T>
void BindVertices(Command& cmd,
                  HostBuffer& host_buffer,
                  std::initializer_list<typename T::PerVertexData>&& vertices) {
  VertexBufferBuilder<typename T::PerVertexData> vtx_builder;
  vtx_builder.AddVertices(vertices);
  cmd.BindVertices(vtx_builder.CreateVertexBuffer(host_buffer));
}

Matrix MakeAnchorScale(const Point& anchor, Vector2 scale) {
  return Matrix::MakeTranslation({anchor.x, anchor.y, 0}) *
         Matrix::MakeScale(scale) *
         Matrix::MakeTranslation({-anchor.x, -anchor.y, 0});
}

void SetTileMode(SamplerDescriptor* descriptor,
                 const ContentContext& renderer,
                 Entity::TileMode tile_mode) {
  switch (tile_mode) {
    case Entity::TileMode::kDecal:
      if (renderer.GetDeviceCapabilities().SupportsDecalSamplerAddressMode()) {
        descriptor->width_address_mode = SamplerAddressMode::kDecal;
        descriptor->height_address_mode = SamplerAddressMode::kDecal;
      }
      break;
    case Entity::TileMode::kClamp:
      descriptor->width_address_mode = SamplerAddressMode::kClampToEdge;
      descriptor->height_address_mode = SamplerAddressMode::kClampToEdge;
      break;
    case Entity::TileMode::kMirror:
      descriptor->width_address_mode = SamplerAddressMode::kMirror;
      descriptor->height_address_mode = SamplerAddressMode::kMirror;
      break;
    case Entity::TileMode::kRepeat:
      descriptor->width_address_mode = SamplerAddressMode::kRepeat;
      descriptor->height_address_mode = SamplerAddressMode::kRepeat;
      break;
  }
}

/// Makes a subpass that will render the scaled down input and add the
/// transparent gutter required for the blur halo.
std::shared_ptr<Texture> MakeDownsampleSubpass(
    const ContentContext& renderer,
    std::shared_ptr<Texture> input_texture,
    const SamplerDescriptor& sampler_descriptor,
    const Quad& uvs,
    const ISize& subpass_size,
    const Vector2 padding,
    Entity::TileMode tile_mode) {
  ContentContext::SubpassCallback subpass_callback =
      [&](const ContentContext& renderer, RenderPass& pass) {
        HostBuffer& host_buffer = pass.GetTransientsBuffer();

        Command cmd;
        DEBUG_COMMAND_INFO(cmd, "Gaussian blur downsample");
        auto pipeline_options = OptionsFromPass(pass);
        pipeline_options.primitive_type = PrimitiveType::kTriangleStrip;
        cmd.pipeline = renderer.GetTexturePipeline(pipeline_options);

        TextureFillVertexShader::FrameInfo frame_info;
        frame_info.mvp = Matrix::MakeOrthographic(ISize(1, 1));
        frame_info.texture_sampler_y_coord_scale = 1.0;
        frame_info.alpha = 1.0;

        // Insert transparent gutter around the downsampled image so the blur
        // creates a halo effect. This compensates for when the expanded clip
        // region can't give us the full gutter we want.
        Vector2 texture_size = Vector2(input_texture->GetSize());
        Quad guttered_uvs =
            MakeAnchorScale({0.5, 0.5},
                            (texture_size + padding * 2) / texture_size)
                .Transform(uvs);

        BindVertices<TextureFillVertexShader>(
            cmd, host_buffer,
            {
                {Point(0, 0), guttered_uvs[0]},
                {Point(1, 0), guttered_uvs[1]},
                {Point(0, 1), guttered_uvs[2]},
                {Point(1, 1), guttered_uvs[3]},
            });

        SamplerDescriptor linear_sampler_descriptor = sampler_descriptor;
        SetTileMode(&linear_sampler_descriptor, renderer, tile_mode);
        linear_sampler_descriptor.mag_filter = MinMagFilter::kLinear;
        linear_sampler_descriptor.min_filter = MinMagFilter::kLinear;
        TextureFillVertexShader::BindFrameInfo(
            cmd, host_buffer.EmplaceUniform(frame_info));
        TextureFillFragmentShader::BindTextureSampler(
            cmd, input_texture,
            renderer.GetContext()->GetSamplerLibrary()->GetSampler(
                linear_sampler_descriptor));

        pass.AddCommand(std::move(cmd));

        return true;
      };
  std::shared_ptr<Texture> out_texture = renderer.MakeSubpass(
      "Gaussian Blur Filter", subpass_size, subpass_callback);
  return out_texture;
}

std::shared_ptr<Texture> MakeBlurSubpass(
    const ContentContext& renderer,
    std::shared_ptr<Texture> input_texture,
    const SamplerDescriptor& sampler_descriptor,
    Entity::TileMode tile_mode,
    const GaussianBlurFragmentShader::BlurInfo& blur_info) {
  if (blur_info.blur_sigma < kEhCloseEnough) {
    return input_texture;
  }

  // TODO(gaaclarke): This blurs the whole image, but because we know the clip
  //                  region we could focus on just blurring that.
  ISize subpass_size = input_texture->GetSize();
  ContentContext::SubpassCallback subpass_callback =
      [&](const ContentContext& renderer, RenderPass& pass) {
        GaussianBlurVertexShader::FrameInfo frame_info{
            .mvp = Matrix::MakeOrthographic(ISize(1, 1)),
            .texture_sampler_y_coord_scale = 1.0};

        HostBuffer& host_buffer = pass.GetTransientsBuffer();

        Command cmd;
        ContentContextOptions options = OptionsFromPass(pass);
        options.primitive_type = PrimitiveType::kTriangleStrip;

        if (tile_mode == Entity::TileMode::kDecal &&
            !renderer.GetDeviceCapabilities()
                 .SupportsDecalSamplerAddressMode()) {
          cmd.pipeline = renderer.GetGaussianBlurDecalPipeline(options);
        } else {
          cmd.pipeline = renderer.GetGaussianBlurPipeline(options);
        }

        BindVertices<GaussianBlurVertexShader>(cmd, host_buffer,
                                               {
                                                   {Point(0, 0), Point(0, 0)},
                                                   {Point(1, 0), Point(1, 0)},
                                                   {Point(0, 1), Point(0, 1)},
                                                   {Point(1, 1), Point(1, 1)},
                                               });

        SamplerDescriptor linear_sampler_descriptor = sampler_descriptor;
        linear_sampler_descriptor.mag_filter = MinMagFilter::kLinear;
        linear_sampler_descriptor.min_filter = MinMagFilter::kLinear;
        GaussianBlurFragmentShader::BindTextureSampler(
            cmd, input_texture,
            renderer.GetContext()->GetSamplerLibrary()->GetSampler(
                linear_sampler_descriptor));
        GaussianBlurVertexShader::BindFrameInfo(
            cmd, host_buffer.EmplaceUniform(frame_info));
        GaussianBlurFragmentShader::BindBlurInfo(
            cmd, host_buffer.EmplaceUniform(blur_info));
        pass.AddCommand(std::move(cmd));

        return true;
      };
  std::shared_ptr<Texture> out_texture = renderer.MakeSubpass(
      "Gaussian Blur Filter", subpass_size, subpass_callback);
  return out_texture;
}

}  // namespace

GaussianBlurFilterContents::GaussianBlurFilterContents(
    Scalar sigma_x,
    Scalar sigma_y,
    Entity::TileMode tile_mode)
    : sigma_x_(sigma_x), sigma_y_(sigma_y), tile_mode_(tile_mode) {}

// This value was extracted from Skia, see:
//  * https://github.com/google/skia/blob/d29cc3fe182f6e8a8539004a6a4ee8251677a6fd/src/gpu/ganesh/GrBlurUtils.cpp#L2561-L2576
//  * https://github.com/google/skia/blob/d29cc3fe182f6e8a8539004a6a4ee8251677a6fd/src/gpu/BlurUtils.h#L57
Scalar GaussianBlurFilterContents::CalculateScale(Scalar sigma) {
  if (sigma <= 4) {
    return 1.0;
  }
  return 4.0 / sigma;
};

std::optional<Rect> GaussianBlurFilterContents::GetFilterSourceCoverage(
    const Matrix& effect_transform,
    const Rect& output_limit) const {
  Vector2 scaled_sigma = {ScaleSigma(sigma_x_), ScaleSigma(sigma_y_)};
  Vector2 blur_radius = {CalculateBlurRadius(scaled_sigma.x),
                         CalculateBlurRadius(scaled_sigma.y)};
  Vector3 blur_radii =
      effect_transform.Basis() * Vector3{blur_radius.x, blur_radius.y, 0.0};
  return output_limit.Expand(Point(blur_radii.x, blur_radii.y));
}

std::optional<Rect> GaussianBlurFilterContents::GetFilterCoverage(
    const FilterInput::Vector& inputs,
    const Entity& entity,
    const Matrix& effect_transform) const {
  if (inputs.empty()) {
    return {};
  }

  std::optional<Rect> input_coverage = inputs[0]->GetCoverage(entity);
  if (!input_coverage.has_value()) {
    return {};
  }

  Vector2 scaled_sigma = {ScaleSigma(sigma_x_), ScaleSigma(sigma_y_)};
  Vector2 blur_radius = {CalculateBlurRadius(scaled_sigma.x),
                         CalculateBlurRadius(scaled_sigma.y)};
  Vector3 blur_radii =
      (inputs[0]->GetTransform(entity).Basis() * effect_transform.Basis() *
       Vector3{blur_radius.x, blur_radius.y, 0.0})
          .Abs();
  return input_coverage.value().Expand(Point(blur_radii.x, blur_radii.y));
}

std::optional<Entity> GaussianBlurFilterContents::RenderFilter(
    const FilterInput::Vector& inputs,
    const ContentContext& renderer,
    const Entity& entity,
    const Matrix& effect_transform,
    const Rect& coverage,
    const std::optional<Rect>& coverage_hint) const {
  if (inputs.empty()) {
    return std::nullopt;
  }

  Vector2 scaled_sigma = {ScaleSigma(sigma_x_), ScaleSigma(sigma_y_)};
  Vector2 blur_radius = {CalculateBlurRadius(scaled_sigma.x),
                         CalculateBlurRadius(scaled_sigma.y)};
  Vector2 padding(ceil(blur_radius.x), ceil(blur_radius.y));

  // Apply as much of the desired padding as possible from the source. This may
  // be ignored so must be accounted for in the downsample pass by adding a
  // transparent gutter.
  std::optional<Rect> expanded_coverage_hint = ExpandCoverageHint(
      coverage_hint, entity.GetTransform() * effect_transform, padding);
  // TODO(gaaclarke): How much of the gutter is thrown away can be used to
  //                  adjust the padding that is added in the downsample pass.
  //                  For example, if we get all the padding we requested from
  //                  the expanded_coverage_hint, there is no need to add a
  //                  transparent gutter.

  std::optional<Snapshot> input_snapshot =
      inputs[0]->GetSnapshot("GaussianBlur", renderer, entity,
                             /*coverage_limit=*/expanded_coverage_hint);
  if (!input_snapshot.has_value()) {
    return std::nullopt;
  }

  if (scaled_sigma.x < kEhCloseEnough && scaled_sigma.y < kEhCloseEnough) {
    return Entity::FromSnapshot(input_snapshot.value(), entity.GetBlendMode(),
                                entity.GetClipDepth());  // No blur to render.
  }

  Scalar desired_scalar =
      std::min(CalculateScale(scaled_sigma.x), CalculateScale(scaled_sigma.y));
  // TODO(jonahwilliams): If desired_scalar is 1.0 and we fully acquired the
  // gutter from the expanded_coverage_hint, we can skip the downsample pass.
  // pass.
  Vector2 downsample_scalar(desired_scalar, desired_scalar);
  Vector2 padded_size =
      Vector2(input_snapshot->texture->GetSize()) + 2.0 * padding;
  Vector2 downsampled_size = padded_size * downsample_scalar;
  // TODO(gaaclarke): I don't think we are correctly handling this fractional
  //                  amount we are throwing away.
  ISize subpass_size =
      ISize(round(downsampled_size.x), round(downsampled_size.y));
  Vector2 effective_scalar = subpass_size / padded_size;

  Quad uvs =
      CalculateUVs(inputs[0], entity, input_snapshot->texture->GetSize());

  std::shared_ptr<Texture> pass1_out_texture = MakeDownsampleSubpass(
      renderer, input_snapshot->texture, input_snapshot->sampler_descriptor,
      uvs, subpass_size, padding, tile_mode_);

  Vector2 pass1_pixel_size = 1.0 / Vector2(pass1_out_texture->GetSize());

  std::shared_ptr<Texture> pass2_out_texture =
      MakeBlurSubpass(renderer, pass1_out_texture,
                      input_snapshot->sampler_descriptor, tile_mode_,
                      GaussianBlurFragmentShader::BlurInfo{
                          .blur_uv_offset = Point(0.0, pass1_pixel_size.y),
                          .blur_sigma = scaled_sigma.y * effective_scalar.y,
                          .blur_radius = blur_radius.y * effective_scalar.y,
                          .step_size = 1.0,
                      });

  // TODO(gaaclarke): Make this pass reuse the texture from pass1.
  std::shared_ptr<Texture> pass3_out_texture =
      MakeBlurSubpass(renderer, pass2_out_texture,
                      input_snapshot->sampler_descriptor, tile_mode_,
                      GaussianBlurFragmentShader::BlurInfo{
                          .blur_uv_offset = Point(pass1_pixel_size.x, 0.0),
                          .blur_sigma = scaled_sigma.x * effective_scalar.x,
                          .blur_radius = blur_radius.x * effective_scalar.x,
                          .step_size = 1.0,
                      });

  SamplerDescriptor sampler_desc = MakeSamplerDescriptor(
      MinMagFilter::kLinear, SamplerAddressMode::kClampToEdge);

  return Entity::FromSnapshot(
      Snapshot{
          .texture = pass3_out_texture,
          .transform = input_snapshot->transform *
                       Matrix::MakeTranslation({-padding.x, -padding.y, 0}) *
                       Matrix::MakeScale(1 / effective_scalar),
          .sampler_descriptor = sampler_desc,
          .opacity = input_snapshot->opacity},
      entity.GetBlendMode(), entity.GetClipDepth());
}

Scalar GaussianBlurFilterContents::CalculateBlurRadius(Scalar sigma) {
  return static_cast<Radius>(Sigma(sigma)).radius;
}

Quad GaussianBlurFilterContents::CalculateUVs(
    const std::shared_ptr<FilterInput>& filter_input,
    const Entity& entity,
    const ISize& texture_size) {
  Matrix input_transform = filter_input->GetLocalTransform(entity);
  Rect snapshot_rect =
      Rect::MakeXYWH(0, 0, texture_size.width, texture_size.height);
  Quad coverage_quad = snapshot_rect.GetTransformedPoints(input_transform);

  Matrix uv_transform = Matrix::MakeScale(
      {1.0f / texture_size.width, 1.0f / texture_size.height, 1.0f});
  return uv_transform.Transform(coverage_quad);
}

// This function was calculated by observing Skia's behavior. Its blur at 500
// seemed to be 0.15.  Since we clamp at 500 I solved the quadratic equation
// that puts the minima there and a f(0)=1.
Scalar GaussianBlurFilterContents::ScaleSigma(Scalar sigma) {
  // Limit the kernel size to 1000x1000 pixels, like Skia does.
  Scalar clamped = std::min(sigma, 500.0f);
  constexpr Scalar a = 3.4e-06;
  constexpr Scalar b = -3.4e-3;
  constexpr Scalar c = 1.f;
  Scalar scalar = c + b * clamped + a * clamped * clamped;
  return clamped * scalar;
}

}  // namespace impeller
