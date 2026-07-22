// Terrarium plugin UI: watch the vat that's playing your synth.
//
// The DSP renders the world 1px-per-cell into g_terrariumView after each sim
// tick (see TerrariumShared.hpp); this UI uploads that frame as a texture and
// scales it to the window with nearest-neighbour, same look as terrarium-pico.
#include "DistrhoUI.hpp"
#include "OpenGL.hpp"

#include "TerrariumShared.hpp"
#include "terrarium_core.hpp"

#include <cstring>

START_NAMESPACE_DISTRHO

class TerrariumUI : public UI {
public:
  TerrariumUI() : UI(W * 3, H * 3) {
    setGeometryConstraints(W, H, true);  // keep the world's aspect
    local_.resize((size_t)W * H, 0xFF000000u);
    g_terrariumView.viewers.fetch_add(1, std::memory_order_release);
  }

  ~TerrariumUI() override {
    g_terrariumView.viewers.fetch_sub(1, std::memory_order_release);
    if (tex_ != 0) glDeleteTextures(1, &tex_);
  }

protected:
  void parameterChanged(uint32_t, float) override {}

  void uiIdle() override {
    // Repaint only when the sim actually ticked (tps is 1..30, not 60fps).
    if (g_terrariumView.dirty.load(std::memory_order_acquire)) repaint();
  }

  void onDisplay() override {
    // Grab the latest frame, if any.
    {
      std::lock_guard<std::mutex> lk(g_terrariumView.mutex);
      if (g_terrariumView.pixels.size() == local_.size()) {
        std::memcpy(local_.data(), g_terrariumView.pixels.data(),
                    local_.size() * sizeof(uint32_t));
      }
      g_terrariumView.dirty.store(false, std::memory_order_release);
    }

    if (tex_ == 0) {
      glGenTextures(1, &tex_);
      glBindTexture(GL_TEXTURE_2D, tex_);
      // Nearest: crisp cells, exactly like the pico build.
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
      glBindTexture(GL_TEXTURE_2D, tex_);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, local_.data());

    const float uiW = (float)getWidth();
    const float uiH = (float)getHeight();

    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(uiW, 0.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(uiW, uiH);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, uiH);
    glEnd();
    glDisable(GL_TEXTURE_2D);
  }

private:
  GLuint tex_ = 0;
  std::vector<uint32_t> local_;

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TerrariumUI)
};

UI* createUI() { return new TerrariumUI(); }

END_NAMESPACE_DISTRHO
