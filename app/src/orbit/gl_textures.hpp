// Wings TV app - Textures in OpenGL, for the console and the PC preview.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/gl_batch.hpp"
#include "orbit/art.hpp"

#include <GL/glcorearb.h>

namespace orbit
{

class GlTextures final : public TextureSink
{
  public:
    explicit GlTextures(hui::gfx::GlBatch &batch) : batch_(batch)
    {
    }
    std::uint32_t create(const image::Pixels &pixels) override
    {
        if (pixels.empty())
            return 0;
        return batch_.create_texture(pixels.width, pixels.height, pixels.rgba.data());
    }
    void destroy(std::uint32_t texture) override
    {
        const GLuint name = texture;
        if (name != 0)
            glDeleteTextures(1, &name);
    }

  private:
    hui::gfx::GlBatch &batch_;
};

} // namespace orbit
