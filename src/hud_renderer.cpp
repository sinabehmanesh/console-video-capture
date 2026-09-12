#include "hud_renderer.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

constexpr UINT kHudWidth = 590;
constexpr UINT kHudHeight = 390;
constexpr UINT kHudX = 10;
constexpr UINT kHudY = 10;
constexpr UINT kGlyphWidth = 5;
constexpr UINT kGlyphHeight = 7;
constexpr UINT kGlyphScale = 2;
constexpr UINT kGlyphAdvance = 12;
constexpr UINT kLineAdvance = 18;
constexpr UINT kTextOriginX = 8;
constexpr UINT kTextOriginY = 7;

constexpr char kVertexShaderSource[] = R"(
struct VSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertex_id : SV_VertexID) {
    VSOut output;

    if (vertex_id == 0) {
        output.position = float4(-1.0, -1.0, 0.0, 1.0);
        output.uv = float2(0.0, 1.0);
    } else if (vertex_id == 1) {
        output.position = float4(-1.0, 3.0, 0.0, 1.0);
        output.uv = float2(0.0, -1.0);
    } else {
        output.position = float4(3.0, -1.0, 0.0, 1.0);
        output.uv = float2(2.0, 1.0);
    }

    return output;
}
)";

constexpr char kPixelShaderSource[] = R"(
Texture2D<float4> hud_texture : register(t0);

struct PSIn {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSIn input) : SV_TARGET {
    uint width = 0;
    uint height = 0;
    hud_texture.GetDimensions(width, height);

    uint x = min((uint)(saturate(input.uv.x) * width), width - 1);
    uint y = min((uint)(saturate(input.uv.y) * height), height - 1);
    return hud_texture.Load(int3(x, y, 0));
}
)";

void throw_if_failed(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(message);
}

ID3DBlob* compile_shader(const char* source, const char* entry_point, const char* target) {
    ID3DBlob* shader_blob = nullptr;
    ID3DBlob* error_blob = nullptr;

    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry_point,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &shader_blob,
        &error_blob
    );

    if (FAILED(result)) {
        std::string message = "Failed to compile HUD shader";
        if (error_blob != nullptr) {
            message += ": ";
            message.append(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
            error_blob->Release();
        }
        if (shader_blob != nullptr) shader_blob->Release();
        throw std::runtime_error(message);
    }

    if (error_blob != nullptr) error_blob->Release();
    return shader_blob;
}

using Glyph = std::array<std::uint8_t, kGlyphHeight>;

Glyph glyph_rows(char character) {
    switch (character) {
    case '0': return {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110};
    case '1': return {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110};
    case '2': return {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111};
    case '3': return {0b11110,0b00001,0b00001,0b01110,0b00001,0b00001,0b11110};
    case '4': return {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010};
    case '5': return {0b11111,0b10000,0b10000,0b11110,0b00001,0b00001,0b11110};
    case '6': return {0b01110,0b10000,0b10000,0b11110,0b10001,0b10001,0b01110};
    case '7': return {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000};
    case '8': return {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110};
    case '9': return {0b01110,0b10001,0b10001,0b01111,0b00001,0b00001,0b01110};
    case 'A': return {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001};
    case 'B': return {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110};
    case 'C': return {0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110};
    case 'D': return {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110};
    case 'E': return {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111};
    case 'F': return {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000};
    case 'G': return {0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01110};
    case 'H': return {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001};
    case 'I': return {0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110};
    case 'J': return {0b00111,0b00010,0b00010,0b00010,0b00010,0b10010,0b01100};
    case 'K': return {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001};
    case 'L': return {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111};
    case 'M': return {0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001};
    case 'N': return {0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001};
    case 'O': return {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110};
    case 'P': return {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000};
    case 'Q': return {0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101};
    case 'R': return {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001};
    case 'S': return {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110};
    case 'T': return {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100};
    case 'U': return {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110};
    case 'V': return {0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100};
    case 'W': return {0b10001,0b10001,0b10001,0b10101,0b10101,0b10101,0b01010};
    case 'X': return {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001};
    case 'Y': return {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100};
    case 'Z': return {0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111};
    case 'x': return {0b00000,0b00000,0b10001,0b01010,0b00100,0b01010,0b10001};
    case ':': return {0b00000,0b00100,0b00100,0b00000,0b00100,0b00100,0b00000};
    case '/': return {0b00001,0b00010,0b00010,0b00100,0b01000,0b01000,0b10000};
    case '+': return {0b00000,0b00100,0b00100,0b11111,0b00100,0b00100,0b00000};
    case '-': return {0b00000,0b00000,0b00000,0b11111,0b00000,0b00000,0b00000};
    case '.': return {0b00000,0b00000,0b00000,0b00000,0b00000,0b00110,0b00110};
    default: return {0,0,0,0,0,0,0};
    }
}

void write_pixel(std::vector<unsigned char>& pixels, UINT x, UINT y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    if (x >= kHudWidth || y >= kHudHeight) return;
    const std::size_t index = (static_cast<std::size_t>(y) * kHudWidth + x) * 4;
    pixels[index+0]=r; pixels[index+1]=g; pixels[index+2]=b; pixels[index+3]=a;
}

std::string build_help_text(const std::string& status_text) {
    return status_text + "  H CLOSE HELP\n"
        "\n"
        "CONTROLS\n"
        "F11  FULLSCREEN\n"
        "ESC  LEAVE FULLSCREEN\n"
        "R    CYCLE OUTPUT SIZE\n"
        "A    TOGGLE ASPECT 4:3 / 16:9\n"
        "Q    CYCLE SCALE FILTER\n"
        "U    CYCLE CHROMA RECONSTRUCTION\n"
        "C    BT.601 / BT.709\n"
        "L    LIMITED / FULL RANGE\n"
        "B    BRIGHTNESS   SHIFT+B DECREASE\n"
        "K    CONTRAST     SHIFT+K DECREASE\n"
        "G    GAMMA        SHIFT+G DECREASE\n"
        "S    SATURATION   SHIFT+S DECREASE\n"
        "0    RESET IMAGE SETTINGS\n"
        "M    CYCLE DISPLAY MODE\n"
        "1    FIT ASPECT RATIO\n"
        "2    FIXED RESOLUTION\n"
        "3    STRETCH\n"
        "H    TOGGLE HELP\n"
        "\n"
        "CAPTURE MODE IS SELECTED AT STARTUP";
}

void draw_background(std::vector<unsigned char>& pixels, UINT width, UINT height, unsigned char alpha) {
    width = std::min(width, kHudWidth); height = std::min(height, kHudHeight);
    for (UINT y=0;y<height;++y) for (UINT x=0;x<width;++x) write_pixel(pixels,x,y,0,0,0,alpha);
}

} // namespace

HudRenderer::~HudRenderer(){ release_resources(); }

void HudRenderer::release_resources(){
    if(blend_state_){blend_state_->Release();blend_state_=nullptr;}
    if(shader_resource_view_){shader_resource_view_->Release();shader_resource_view_=nullptr;}
    if(texture_){texture_->Release();texture_=nullptr;}
    if(pixel_shader_){pixel_shader_->Release();pixel_shader_=nullptr;}
    if(vertex_shader_){vertex_shader_->Release();vertex_shader_=nullptr;}
}

void HudRenderer::initialize(ID3D11Device* device){
    if(!device) throw std::runtime_error("Cannot initialize HUD renderer without a D3D11 device");
    release_resources();
    ID3DBlob* vb=compile_shader(kVertexShaderSource,"main","vs_5_0");
    const HRESULT vr=device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vertex_shader_); vb->Release(); throw_if_failed(vr,"Failed to create HUD vertex shader");
    ID3DBlob* pb=compile_shader(kPixelShaderSource,"main","ps_5_0");
    const HRESULT pr=device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&pixel_shader_); pb->Release(); throw_if_failed(pr,"Failed to create HUD pixel shader");
    D3D11_TEXTURE2D_DESC td{}; td.Width=kHudWidth; td.Height=kHudHeight; td.MipLevels=1; td.ArraySize=1; td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DYNAMIC; td.BindFlags=D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    throw_if_failed(device->CreateTexture2D(&td,nullptr,&texture_),"Failed to create HUD texture");
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=td.Format; sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
    throw_if_failed(device->CreateShaderResourceView(texture_,&sd,&shader_resource_view_),"Failed to create HUD shader resource view");
    D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable=TRUE; bd.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD; bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD; bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    throw_if_failed(device->CreateBlendState(&bd,&blend_state_),"Failed to create HUD blend state");
    pixels_.resize(static_cast<std::size_t>(kHudWidth)*kHudHeight*4); dirty_=true; help_visible_=false; h_was_down_=false;
}

void HudRenderer::set_text(std::string text){ if(text_==text)return; text_=std::move(text); dirty_=true; }

void HudRenderer::update_texture(ID3D11DeviceContext* context){
    std::fill(pixels_.begin(),pixels_.end(),static_cast<unsigned char>(0));
    const std::string display_text=help_visible_?build_help_text(text_):text_+"  H HELP";
    if(help_visible_) draw_background(pixels_,570,385,205); else { const UINT w=std::min(kHudWidth,kTextOriginX*2+static_cast<UINT>(display_text.size())*kGlyphAdvance); draw_background(pixels_,w,28,140); }
    UINT cx=kTextOriginX, cy=kTextOriginY;
    for(char ch:display_text){
        if(ch=='\n'){cx=kTextOriginX;cy+=kLineAdvance;if(cy+kGlyphHeight*kGlyphScale>=kHudHeight)break;continue;}
        const auto rows=glyph_rows(ch);
        for(UINT gy=0;gy<kGlyphHeight;++gy) for(UINT gx=0;gx<kGlyphWidth;++gx){ const std::uint8_t bit=static_cast<std::uint8_t>(1u<<(kGlyphWidth-1-gx)); if((rows[gy]&bit)==0)continue; for(UINT sy=0;sy<kGlyphScale;++sy) for(UINT sx=0;sx<kGlyphScale;++sx) write_pixel(pixels_,cx+gx*kGlyphScale+sx,cy+gy*kGlyphScale+sy,255,255,255,250); }
        cx+=kGlyphAdvance; if(cx>=kHudWidth-kGlyphAdvance){cx=kTextOriginX;cy+=kLineAdvance;if(cy+kGlyphHeight*kGlyphScale>=kHudHeight)break;}
    }
    D3D11_MAPPED_SUBRESOURCE mapped{}; throw_if_failed(context->Map(texture_,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Failed to map HUD texture");
    const std::size_t row_bytes=static_cast<std::size_t>(kHudWidth)*4;
    for(UINT row=0;row<kHudHeight;++row) std::memcpy(static_cast<unsigned char*>(mapped.pData)+static_cast<std::size_t>(row)*mapped.RowPitch,pixels_.data()+static_cast<std::size_t>(row)*row_bytes,row_bytes);
    context->Unmap(texture_,0); dirty_=false;
}

void HudRenderer::render(ID3D11DeviceContext* context, UINT target_width, UINT target_height){
    if(!context||!vertex_shader_||!pixel_shader_||!texture_||!shader_resource_view_||target_width==0||target_height==0)return;
    const bool h_down=(GetAsyncKeyState('H')&0x8000)!=0; if(h_down&&!h_was_down_){help_visible_=!help_visible_;dirty_=true;} h_was_down_=h_down;
    if(dirty_) update_texture(context);
    D3D11_VIEWPORT vp{}; vp.TopLeftX=static_cast<float>(kHudX); vp.TopLeftY=static_cast<float>(kHudY); vp.Width=static_cast<float>(std::min(kHudWidth,target_width)); vp.Height=static_cast<float>(std::min(kHudHeight,target_height)); vp.MinDepth=0.0f; vp.MaxDepth=1.0f;
    context->RSSetViewports(1,&vp); context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); context->VSSetShader(vertex_shader_,nullptr,0); context->PSSetShader(pixel_shader_,nullptr,0); context->PSSetShaderResources(0,1,&shader_resource_view_);
    constexpr float bf[4]={0,0,0,0}; context->OMSetBlendState(blend_state_,bf,0xffffffffu); context->Draw(3,0); context->OMSetBlendState(nullptr,bf,0xffffffffu);
    ID3D11ShaderResourceView* null_srv=nullptr; context->PSSetShaderResources(0,1,&null_srv);
}
