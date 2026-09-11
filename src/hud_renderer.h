#pragma once

#include <d3d11.h>

#include <string>
#include <utility>
#include <vector>

class HudRenderer {
public:
    HudRenderer() = default;
    ~HudRenderer();

    HudRenderer(const HudRenderer&) = delete;
    HudRenderer& operator=(const HudRenderer&) = delete;

    void initialize(ID3D11Device* device);
    void set_text(std::string text);
    void render(ID3D11DeviceContext* context, UINT target_width, UINT target_height);

private:
    void update_texture(ID3D11DeviceContext* context);
    void release_resources();

    ID3D11VertexShader* vertex_shader_ = nullptr;
    ID3D11PixelShader* pixel_shader_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    ID3D11ShaderResourceView* shader_resource_view_ = nullptr;
    ID3D11BlendState* blend_state_ = nullptr;

    std::string text_;
    std::vector<unsigned char> pixels_;
    bool dirty_ = true;
};
