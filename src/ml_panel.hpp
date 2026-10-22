#pragma once
#include "ml_inference.hpp"
namespace rfmon::ml {
void draw_panel(Inference& inference,bool connected,const std::string& band);
void draw_stft_preview(const Result& result);
// Call on the GUI thread before destroying its OpenGL context.
void release_preview_texture();
}
