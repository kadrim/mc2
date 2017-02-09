#include <vector>
#include <map>
#include <algorithm>

#include "gameos.hpp"
#include "font3d.hpp"
#include "gos_font.h"

#ifdef LINUX_BUILD
#include <cstdarg>
#endif

#include "stdlib_win.h"

#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/Image.h"
#include "utils/vec.h"

#include "gos_render.h"

class gosRenderer;
class gosFont;

static const DWORD INVALID_TEXTURE_ID = 0;

static gosRenderer* g_gos_renderer = NULL;

gosRenderer* getGosRenderer() {
    return g_gos_renderer;
}

struct gosTextureInfo {
    int width_;
    int height_;
    gos_TextureFormat format_;
};

class gosShaderMaterial {
    public:
        static gosShaderMaterial* load(const char* shader) {
            gosASSERT(shader);
            gosShaderMaterial* pmat = new gosShaderMaterial();
            char vs[256];
            char ps[256];
            snprintf(vs, 255, "shaders/%s.vert", shader);
            snprintf(ps, 255, "shaders/%s.frag", shader);
            pmat->program_ = glsl_program::makeProgram(shader, vs, ps);
            if(!pmat->program_) {
                SPEW(("SHADERS", "Failed to create %s material\n", shader));
                delete pmat;
                return NULL;
            }
            
            pmat->name_ = new char[strlen(shader) + 1];
            strcpy(pmat->name_, shader);

            pmat->pos_loc = pmat->program_->getAttribLocation("pos");
            pmat->color_loc = pmat->program_->getAttribLocation("color");
            pmat->texcoord_loc = pmat->program_->getAttribLocation("texcoord");

            return pmat;
        }

        static void destroy(gosShaderMaterial* pmat) {
            gosASSERT(pmat);
            if(pmat->program_) {
                glsl_program::deleteProgram(pmat->name_);
                pmat->program_ = 0;
            }

            delete[] pmat->name_;
            pmat->name_ = 0;
        }

        void applyVertexDeclaration() {
            
            const int stride = sizeof(gos_VERTEX);
            
            // gos_VERTEX structure
	        //float x,y;
	        //float z;
	        //float rhw;
	        //DWORD argb;
	        //DWORD frgb;
	        //float u,v;	

            gosASSERT(pos_loc >= 0);
            glEnableVertexAttribArray(pos_loc);
            glVertexAttribPointer(pos_loc, 4, GL_FLOAT, GL_FALSE, stride, (void*)0);

            if(color_loc != -1) {
                glEnableVertexAttribArray(color_loc);
                glVertexAttribPointer(color_loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, 
                        BUFFER_OFFSET(4*sizeof(float)));
            }

            if(texcoord_loc != -1) {
                glEnableVertexAttribArray(texcoord_loc);
                glVertexAttribPointer(texcoord_loc, 2, GL_FLOAT, GL_FALSE, stride, 
                        BUFFER_OFFSET(4*sizeof(float) + 2*sizeof(uint32_t)));
            }
        }

        bool setSamplerUnit(const char* sampler_name, uint32_t unit) {
            gosASSERT(sampler_name);
            // TODO: may also check that current program is equal to our program
            if(program_->samplers_.count(sampler_name)) {
                glUniform1i(program_->samplers_[sampler_name]->index_, unit);
                return true;
            }
            return false;
        }

        bool setTransform(const mat4& m) {
            program_->setMat4("mvp", m);
            return true;
        }

        void apply() {
            gosASSERT(program_);
            program_->apply();
        }

        // TODO: think how to not expose this
        glsl_program* getShader() { return program_; }

        void end() {

            glDisableVertexAttribArray(pos_loc);

            if(color_loc != -1) {
                glDisableVertexAttribArray(color_loc);
            }

            if(texcoord_loc != -1) {
                glDisableVertexAttribArray(texcoord_loc);
            }

            glUseProgram(0);
        }

    private:
        gosShaderMaterial():
            program_(NULL)
            , name_(NULL)
            , pos_loc(-1)
            , color_loc(-1)
            , texcoord_loc(-1)
        {
        }

        glsl_program* program_;
        char* name_;
        GLint pos_loc;
        GLint color_loc;
        GLint texcoord_loc;
};

class gosMesh {
    public:
        typedef WORD INDEX_TYPE;

        static gosMesh* makeMesh(gosPRIMITIVETYPE prim_type, int vertex_capacity, int index_capacity = 0) {
            GLuint vb = makeBuffer(GL_ARRAY_BUFFER, 0, sizeof(gos_VERTEX)*vertex_capacity, GL_DYNAMIC_DRAW);
            if(!vb)
                return NULL;

            GLuint ib = 0;
            if(index_capacity > 0) {
                ib = makeBuffer(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(INDEX_TYPE)*index_capacity, GL_DYNAMIC_DRAW);
                if(!ib)
                    return NULL;
            }

            gosMesh* mesh = new gosMesh(prim_type, vertex_capacity, index_capacity);
            mesh->vb_ = vb;
            mesh->ib_ = ib;
            mesh->pvertex_data_ = new gos_VERTEX[vertex_capacity];
            mesh->pindex_data_ = new INDEX_TYPE[index_capacity];
            return mesh;
        }

        static void destroy(gosMesh* pmesh) {

            gosASSERT(pmesh);

            delete[] pmesh->pvertex_data_;
            delete[] pmesh->pindex_data_;

            GLuint b[] = {pmesh->vb_, pmesh->ib_};
            glDeleteBuffers(sizeof(b)/sizeof(b[0]), b);
        }

        bool addVertices(gos_VERTEX* vertices, int count) {
            if(num_vertices_ + count <= vertex_capacity_) {
                memcpy(pvertex_data_ + num_vertices_, vertices, sizeof(gos_VERTEX)*count);
                num_vertices_ += count;
                return true;
            }
            return false;
        }

        bool addIndices(INDEX_TYPE* indices, int count) {
            if(num_indices_ + count <= index_capacity_) {
                memcpy(pindex_data_ + num_indices_, indices, sizeof(INDEX_TYPE)*count);
                num_indices_ += count;
                return true;
            }
            return false;
        }

        int getVertexCapacity() const { return vertex_capacity_; }
        int getIndexCapacity() const { return index_capacity_; }
        int getNumVertices() const { return num_vertices_; }
        int getNumIndices() const { return num_indices_; }
        const gos_VERTEX* getVertices() const { return pvertex_data_; }
        const WORD* getIndices() const { return pindex_data_; }

        int getIndexSizeBytes() const { return sizeof(INDEX_TYPE); }

        void rewind() { num_vertices_ = 0; num_indices_ = 0; }

        void draw(gosShaderMaterial* material) const;
        void drawIndexed(gosShaderMaterial* material) const;

    private:

        gosMesh(gosPRIMITIVETYPE prim_type, int vertex_capacity, int index_capacity)
            : vertex_capacity_(vertex_capacity)
            , index_capacity_(index_capacity)
            , num_vertices_(0)
            , num_indices_(0)
            , pvertex_data_(NULL)    
            , pindex_data_(NULL)    
            , prim_type_(prim_type)
            , vb_(-1)  
            ,ib_(-1) 
         {
         }

        int vertex_capacity_;
        int index_capacity_;
        int num_vertices_;
        int num_indices_;
        gos_VERTEX* pvertex_data_;
        INDEX_TYPE* pindex_data_;
        gosPRIMITIVETYPE prim_type_;

        GLuint vb_;
        GLuint ib_;
};

void gosMesh::draw(gosShaderMaterial* material) const
{
    gosASSERT(material);

    if(num_vertices_ == 0)
        return;

    updateBuffer(vb_, GL_ARRAY_BUFFER, pvertex_data_, num_vertices_*sizeof(gos_VERTEX), GL_DYNAMIC_DRAW);

    material->apply();

    material->setSamplerUnit("tex1", 0);

	glBindBuffer(GL_ARRAY_BUFFER, vb_);

    material->applyVertexDeclaration();
    CHECK_GL_ERROR;

    GLenum pt = GL_TRIANGLES;
    switch(prim_type_) {
        case PRIMITIVE_POINTLIST:
            pt = GL_POINTS;
            break;
        case PRIMITIVE_LINELIST:
            pt = GL_LINES;
            break;
        case PRIMITIVE_TRIANGLELIST:
            pt = GL_TRIANGLES;
            break;
        default:
            gosASSERT(0 && "Wrong primitive type");
    }

    glDrawArrays(pt, 0, num_vertices_);

    material->end();

	glBindBuffer(GL_ARRAY_BUFFER, 0);

}

void gosMesh::drawIndexed(gosShaderMaterial* material) const
{
    gosASSERT(material);

    if(num_vertices_ == 0)
        return;

    updateBuffer(vb_, GL_ARRAY_BUFFER, pvertex_data_, num_vertices_*sizeof(gos_VERTEX), GL_DYNAMIC_DRAW);
    updateBuffer(ib_, GL_ELEMENT_ARRAY_BUFFER, pindex_data_, num_indices_*sizeof(INDEX_TYPE), GL_DYNAMIC_DRAW);

    material->apply();

    material->setSamplerUnit("tex1", 0);

	glBindBuffer(GL_ARRAY_BUFFER, vb_);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_);
    CHECK_GL_ERROR;

    material->applyVertexDeclaration();
    CHECK_GL_ERROR;

    GLenum pt = GL_TRIANGLES;
    switch(prim_type_) {
        case PRIMITIVE_POINTLIST:
            pt = GL_POINTS;
            break;
        case PRIMITIVE_LINELIST:
            pt = GL_LINES;
            break;
        case PRIMITIVE_TRIANGLELIST:
            pt = GL_TRIANGLES;
            break;
        default:
            gosASSERT(0 && "Wrong primitive type");
    }

    glDrawElements(pt, num_indices_, getIndexSizeBytes()==2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

    material->end();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

}

class gosTexture {
    public:
        gosTexture(gos_TextureFormat fmt, const char* fname, DWORD hints, BYTE* pdata, DWORD size, bool from_memory)
        {

	        //if(fmt == gos_Texture_Detect || /*fmt == gos_Texture_Keyed ||*/ fmt == gos_Texture_Bump || fmt == gos_Texture_Normal)
            //     PAUSE((""));

            format_ = fmt;
            if(fname) {
                filename_ = new char[strlen(fname)+1];
                strcpy(filename_, fname);
            } else {
                filename_ = 0;
            }
            texname_ = NULL;

            hints_ = hints;

            plocked_area_ = NULL;

            size_ = 0;
            pcompdata_ = NULL;
            if(size) {
                size_ = size;
                pcompdata_ = new BYTE[size];
                memcpy(pcompdata_, pdata, size);
            }

            is_locked_ = false;
            is_from_memory_ = from_memory;
        }

        gosTexture(gos_TextureFormat fmt, DWORD hints, DWORD w, DWORD h, const char* texname)
        {
	        //if(fmt == gos_Texture_Detect /*|| fmt == gos_Texture_Keyed*/ || fmt == gos_Texture_Bump || fmt == gos_Texture_Normal)
            //     PAUSE((""));

            format_ = fmt;
            if(texname) {
                texname_ = new char[strlen(texname)+1];
                strcpy(texname_, texname);
            } else {
                texname_ = 0;
            }
            filename_ = NULL;
            hints_ = hints;

            plocked_area_ = NULL;

            size_ = 0;
            pcompdata_ = NULL;
            tex_.w = w;
            tex_.h = h;

            is_locked_ = false;
            is_from_memory_ = true;
        }

        bool createHardwareTexture();

        ~gosTexture() {

            //SPEW(("Destroying texture: %s\n", filename_));

            gosASSERT(is_locked_ == false);

            if(pcompdata_)
                delete[] pcompdata_;
            if(filename_)
                delete[] filename_;
            if(texname_)
                delete[] texname_;

            destroyTexture(&tex_);
        }

        uint32_t getTextureId() const { return tex_.id; }
        TexType getTextureType() const { return tex_.type_; }

        BYTE* Lock(int mipl_level, bool is_read_only, int* pitch) {
            gosASSERT(is_locked_ == false);
            is_locked_ = true;
            // TODO:
            gosASSERT(pitch);
            *pitch = tex_.w;

            gosASSERT(!plocked_area_);
#if 0 
            glBindTexture(GL_TEXTURE_2D, tex_.id);
            GLint pack_row_length;
            GLint pack_alignment;
            glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
            glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
            glBindTexture(GL_TEXTURE_2D, 0);
#endif
            // always return rgba8 formatted data
            lock_type_read_only_ = is_read_only;
            const uint32_t ts = tex_.w*tex_.h * getTexFormatPixelSize(TF_RGBA8);
            plocked_area_ = new BYTE[ts];
            getTextureData(tex_, 0, plocked_area_, TF_RGBA8);
            for(int y=0;y<tex_.h;++y) {
                for(int x=0;x<tex_.w;++x) {
                    DWORD rgba = ((DWORD*)plocked_area_)[tex_.w*y + x];
                    DWORD r = rgba&0xff;
                    DWORD g = (rgba&0xff00)>>8;
                    DWORD b = (rgba&0xff0000)>>16;
                    DWORD a = (rgba&0xff000000)>>24;
                    DWORD bgra = (a<<24) | (r<<16) | (g<<8) | b;
                    ((DWORD*)plocked_area_)[tex_.w*y + x] = bgra;
                }
            }
            return plocked_area_;
        }

        void Unlock() {
            gosASSERT(is_locked_ == true);
        
            if(!lock_type_read_only_) {
                for(int y=0;y<tex_.h;++y) {
                    for(int x=0;x<tex_.w;++x) {
                        DWORD bgra = ((DWORD*)plocked_area_)[tex_.w*y + x];
                        DWORD b = bgra&0xff;
                        DWORD g = (bgra&0xff00)>>8;
                        DWORD r = (bgra&0xff0000)>>16;
                        DWORD a = (bgra&0xff000000)>>24;
                        DWORD argb = (a<<24) | (b<<16) | (g<<8) | r;
                        ((DWORD*)plocked_area_)[tex_.w*y + x] = argb;
                    }
                }
                updateTexture(tex_, plocked_area_, TF_RGBA8);
            }

            delete[] plocked_area_;
            plocked_area_ = NULL;

            is_locked_ = false;
        }

        void getTextureInfo(gosTextureInfo* texinfo) const {
            gosASSERT(texinfo);
            texinfo->width_ = tex_.w;
            texinfo->height_ = tex_.h;
            texinfo->format_ = format_;
        }

    private:
        BYTE* pcompdata_;
        BYTE* plocked_area_;
        DWORD size_;
        Texture tex_;

        gos_TextureFormat format_;
        char* filename_;
        char* texname_;
        DWORD hints_;

        bool is_locked_;
        bool lock_type_read_only_;
        bool is_from_memory_; // not loaded from file
};

struct gosTextAttribs {
    HGOSFONT3D FontHandle;
    DWORD Foreground;
    float Size;
    bool WordWrap;
    bool Proportional;
    bool Bold;
    bool Italic;
    DWORD WrapType;
    bool DisableEmbeddedCodes;
};

static void makeKindaSolid(Image& img) {
    // have to do this, otherwise texutre with zero alpha could be drawn with alpha blend enabled, evel though logically aplha blend should not be enabled!
    // (happens when drawing terrain, see TerrainQuad::draw() case when no detail and no owerlay bu t isCement is true)
    DWORD* pixels = (DWORD*)img.getPixels();
    for(int y=0;y<img.getHeight(); ++y) {
        for(int x=0;x<img.getWidth(); ++x) {
            DWORD pix = pixels[y*img.getWidth() + x];
            pixels[y*img.getWidth() + x] = pix | 0xff000000;
        }
    }
}

static bool doesLookLikeAlpha(const Image& img) {
    gosASSERT(img.getFormat() == FORMAT_RGBA8);

    DWORD* pixels = (DWORD*)img.getPixels();
    for(int y=0;y<img.getHeight(); ++y) {
        for(int x=0;x<img.getWidth(); ++x) {
            DWORD pix = pixels[y*img.getWidth() + x];
            if((0xFF000000 & pix) != 0xFF000000)
                return true;
        }
    }
    return false;
}

static gos_TextureFormat convertIfNecessary(Image& img, gos_TextureFormat gos_format) {

    const bool has_alpha_channel = FORMAT_RGBA8 == img.getFormat();

    if(gos_format == gos_Texture_Detect) {
        bool has_alpha = has_alpha_channel ? doesLookLikeAlpha(img) : false;
        gos_format = has_alpha ? gos_Texture_Alpha : gos_Texture_Solid;
    }

    if(gos_format == gos_Texture_Solid && has_alpha_channel)
        makeKindaSolid(img);

    return gos_format;
}

bool gosTexture::createHardwareTexture() {

    if(!is_from_memory_) {

        gosASSERT(filename_);
        SPEW(("DBG", "creating texture: %s\n", filename_));

        Image img;
        if(!img.loadFromFile(filename_)) {
            SPEW(("DBG", "failed to load texture from file: %s\n", filename_));
            return false;
        }

        // check for only those formats, because lock.unlock may incorrectly work with different channes size (e.g. 16 or 32bit or floats)
        FORMAT img_fmt = img.getFormat();
        if(img_fmt != FORMAT_RGB8 && img_fmt != FORMAT_RGBA8) {
            STOP(("Unsupported texture format when loading %s\n", filename_));
        }

        TexFormat tf = img_fmt == FORMAT_RGB8 ? TF_RGB8 : TF_RGBA8;

        format_ = convertIfNecessary(img, format_);

        tex_ = create2DTexture(img.getWidth(), img.getHeight(), tf, img.getPixels());
        return tex_.isValid();

    } else if(pcompdata_ && size_ > 0) {

        // TODO: this is texture from memory, so maybe do not load it from file eh?

        Image img;
        if(!img.loadTGA(pcompdata_, size_)) {
            SPEW(("DBG", "failed to load texture from data, filename: %s, texname: %s\n", filename_? filename_ : "NO FILENAME", texname_?texname_:"NO TEXNAME"));
            return false;
        }

        FORMAT img_fmt = img.getFormat();

        if(img_fmt != FORMAT_RGB8 && img_fmt != FORMAT_RGBA8) {
            STOP(("Unsupported texture format when loading %s\n", filename_));
        }

        TexFormat tf = img_fmt == FORMAT_RGB8 ? TF_RGB8 : TF_RGBA8;

        format_ = convertIfNecessary(img, format_);

        tex_ = create2DTexture(img.getWidth(), img.getHeight(), tf, img.getPixels());
        return tex_.isValid();
    } else {
        gosASSERT(tex_.w >0 && tex_.h > 0);

        TexFormat tf = TF_RGBA8; // TODO: check format_ and do appropriate stuff
        DWORD* pdata = new DWORD[tex_.w*tex_.h];
        for(int i=0;i<tex_.w*tex_.h;++i)
            pdata[i] = 0xFF00FFFF;
        tex_ = create2DTexture(tex_.w, tex_.h, tf, (const uint8_t*)pdata);
        delete[] pdata;
        return tex_.isValid();
    }

}

////////////////////////////////////////////////////////////////////////////////
class gosFont {
    public:
        static gosFont* load(const char* fontFile);
        static void destroy(gosFont* font);

        int getMaxCharWidth() const { return gi_.max_advance_; }
        int getMaxCharHeight() const { return gi_.font_line_skip_; }

        int getCharWidth(int c) const;
        void getCharUV(int c, uint32_t* u, uint32_t* v) const;
        int getCharAdvance(int c) const;

        DWORD getTextureId() const { return tex_id_; }
        const char* getName() const { return font_name_; }

    private:
        gosFont(){};
        // TODO: free texture and other stuff
        ~gosFont(){};

        char* font_name_;
        gosGlyphInfo gi_;
        DWORD tex_id_;
};

////////////////////////////////////////////////////////////////////////////////
class gosRenderer {

    typedef unsigned int RenderState[gos_MaxState];

    public:
        gosRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h) {
            width_ = w;
            height_ = h;
            ctx_h_ = ctx_h;
            win_h_ = win_h;
        }

        DWORD addTexture(gosTexture* texture) {
            gosASSERT(texture);
            textureList_.push_back(texture);
            return textureList_.size()-1;
        }

        DWORD addFont(gosFont* font) {
            gosASSERT(font);
            fontList_.push_back(font);
            return fontList_.size()-1;
        }

        // TODO: do sae as with texture?
        void deleteFont(gosFont* font) {
            // FIXME: bad use object list, with stable ids
            // to not waste space
            
            struct equals_to {
                gosFont* fnt_;
                bool operator()(gosFont* fnt) {
                    return fnt == fnt_;
                }
            };

            equals_to eq;
            eq.fnt_ = font;

            std::vector<gosFont*>::iterator it = 
                std::find_if(fontList_.begin(), fontList_.end(), eq);
            if(it != fontList_.end())
            {
                gosFont* font = *it;
                fontList_.erase(it);
                gosFont::destroy(font);
            }
        }

        gosTexture* getTexture(DWORD texture_id) {
            // TODO: return default texture
            if(texture_id == INVALID_TEXTURE_ID) {
                gosASSERT(0 && "Should not be requested");
                return NULL;
            }
            gosASSERT(textureList_.size() > texture_id);
            gosASSERT(textureList_[texture_id] != 0);
            return textureList_[texture_id];
        }

        void deleteTexture(DWORD texture_id) {
            // FIXME: bad use object list, with stable ids
            // to not waste space
            gosASSERT(textureList_.size() > texture_id);
            delete textureList_[texture_id];
            textureList_[texture_id] = 0;
        }

        gosTextAttribs& getTextAttributes() { return curTextAttribs_; }
        void setTextPos(int x, int y) { curTextPosX_ = x; curTextPosY_ = y; }
        void getTextPos(int& x, int& y) { x = curTextPosX_; y = curTextPosY_; }
        void setTextRegion(int Left, int Top, int Right, int Bottom) {
            curTextLeft_ = Left;
            curTextTop_ = Top;
            curTextRight_ = Right;
            curTextBottom_ = Bottom;
        }

        int getTextRegionWidth() { return curTextRight_ - curTextLeft_; }
        int getTextRegionHeight() { return curTextBottom_ - curTextTop_; }

        void setupViewport(bool FillZ, float ZBuffer, bool FillBG, DWORD BGColor, float top, float left, float bottom, float right, bool ClearStencil = 0, DWORD StencilValue = 0) {

            clearDepth_ = FillZ;
            clearDepthValue_ = ZBuffer;
            clearColor_ = FillBG;
            clearColorValue_ = BGColor;
            clearStencil_ = ClearStencil;
            clearStencilValue_ = StencilValue;
            viewportTop_ = top;
            viewportLeft_ = left;
            viewportBottom_ = bottom;
            viewportRight_ = right;
        }

        void getViewportTransform(float* viewMulX, float* viewMulY, float* viewAddX, float* viewAddY) {
            gosASSERT(viewMulX && viewMulY && viewAddX && viewAddY);
            *viewMulX = (viewportRight_ - viewportLeft_)*width_;
            *viewMulY = (viewportBottom_ - viewportTop_)*height_;
            *viewAddX = viewportLeft_ * width_;
            *viewAddY = viewportTop_ * height_;
        }

        void setRenderState(gos_RenderState RenderState, int Value) {
            renderStates_[RenderState] = Value;
        }

        int getRenderState(gos_RenderState RenderState) const {
            return renderStates_[RenderState];
        }

        void setScreenMode(DWORD width, DWORD height, DWORD bit_depth, bool GotoFullScreen, bool anti_alias) {
            reqWidth = width;
            reqHeight = height;
            reqBitDepth = bit_depth;
            reqAntiAlias = anti_alias;
            reqGotoFullscreen = GotoFullScreen;
            pendingRequest = true;
        }

        void pushRenderStates();
        void popRenderStates();

        void applyRenderStates();

        void drawQuads(gos_VERTEX* vertices, int count);
        void drawLines(gos_VERTEX* vertices, int count);
        void drawPoints(gos_VERTEX* vertices, int count);
        void drawTris(gos_VERTEX* vertices, int count);
        void drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices);
        void drawText(const char* text);

        void beginFrame();
        void endFrame();

        void init();
        void destroy();
        void flush();

        // debug interface
        void setNumDrawCallsToDraw(uint32_t num) { num_draw_calls_to_draw_ = num; }
        uint32_t getNumDrawCallsToDraw() { return num_draw_calls_to_draw_; }
        void setBreakOnDrawCall(bool b_break) { break_on_draw_call_ = b_break; }
        bool getBreakOnDrawCall() { return break_on_draw_call_; }
        void setBreakDrawCall(uint32_t num) { break_draw_call_num_ = num; }

        graphics::RenderContextHandle getRenderContextHandle() { return ctx_h_; }

    private:

        bool beforeDrawCall();
        void afterDrawCall();

        // render target size
        int width_;
        int height_;
        graphics::RenderContextHandle ctx_h_;
        graphics::RenderWindowHandle win_h_;

        // fits vertices into viewport
        mat4 projection_;

        void initRenderStates();

        std::vector<gosTexture*> textureList_;
        std::vector<gosFont*> fontList_;

        DWORD reqWidth;
        DWORD reqHeight;
        DWORD reqBitDepth;
        DWORD reqAntiAlias;
        DWORD reqGotoFullscreen;
        bool pendingRequest;

        // states data
        RenderState curStates_;
        RenderState renderStates_;

        static const int RENDER_STATES_STACK_SIZE = 16;
        int renderStatesStackPointer;
        RenderState statesStack_[RENDER_STATES_STACK_SIZE];
        //

        // text data
        gosTextAttribs curTextAttribs_;

        int curTextPosX_;
        int curTextPosY_;

        int curTextLeft_;
        int curTextTop_;
        int curTextRight_;
        int curTextBottom_;
        //
        
        // viewport config
        bool clearDepth_;
        float clearDepthValue_;
        bool clearColor_;
        DWORD clearColorValue_;
        bool clearStencil_;
        DWORD clearStencilValue_;
        float viewportTop_;
        float viewportLeft_;
        float viewportBottom_;
        float viewportRight_;
        //
        
        gosMesh* quads_;
        gosMesh* tris_;
        gosMesh* indexed_tris_;
        gosMesh* lines_;
        gosMesh* points_;
        gosMesh* text_;
        gosShaderMaterial* basic_material_;
        gosShaderMaterial* basic_tex_material_;
        gosShaderMaterial* text_material_;

        //
        uint32_t num_draw_calls_;
        uint32_t num_draw_calls_to_draw_;
        bool break_on_draw_call_;
        uint32_t break_draw_call_num_;

};

void gosRenderer::init() {
    initRenderStates();

    // x = 1/w; x =2*x - 1;
    // y = 1/h; y= 1- y; y =2*y - 1;
    // z = z;
    projection_ = mat4(
            2.0f / (float)width_, 0, 0.0f, -1.0f,
            0, -2.0f / (float)height_, 0.0f, 1.0f,
            0, 0, 1.0f, 0.0f,
            0, 0, 0.0f, 1.0f);

    // setup viewport
    setupViewport(true, 1.0f, true, 0, 0.0f, 0.0f, 1.0f, 1.0f);

    quads_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*10);
    gosASSERT(quads_);
    tris_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*10);
    gosASSERT(tris_);
    indexed_tris_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*10, 1024*10);
    gosASSERT(indexed_tris_);
    lines_ = gosMesh::makeMesh(PRIMITIVE_LINELIST, 1024*10);
    gosASSERT(lines_);
    points_= gosMesh::makeMesh(PRIMITIVE_POINTLIST, 1024*10);
    gosASSERT(points_);
    text_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 4024 * 6);
    gosASSERT(text_);
    basic_material_ = gosShaderMaterial::load("gos_vertex");
    gosASSERT(basic_material_);
    basic_tex_material_ = gosShaderMaterial::load("gos_tex_vertex");
    gosASSERT(basic_tex_material_);
    text_material_ = gosShaderMaterial::load("gos_text");
    gosASSERT(text_material_);

    pendingRequest = false;

    num_draw_calls_ = 0;
    num_draw_calls_to_draw_ = 0;
    break_on_draw_call_ = false;
    break_draw_call_num_ = 0;

    // add fake texture so that no one will get 0 index, as it is invalid in this game
    DWORD tex_id = gos_NewEmptyTexture( gos_Texture_Solid, "DEBUG_this_is_not_a_real_texture_debug_it!", 1);
    gosASSERT(tex_id == INVALID_TEXTURE_ID);
}

void gosRenderer::destroy() {

    gosMesh::destroy(quads_);
    gosMesh::destroy(tris_);
    gosMesh::destroy(indexed_tris_);
    gosMesh::destroy(lines_);
    gosMesh::destroy(points_);
    gosMesh::destroy(text_);

    gosShaderMaterial::destroy(basic_material_);
    gosShaderMaterial::destroy(basic_tex_material_);
    gosShaderMaterial::destroy(text_material_);

    for(size_t i=0; i<textureList_.size(); i++) {
        delete textureList_[i];
    }
    textureList_.clear();

    for(size_t i=0; i<textureList_.size(); i++) {
        gosFont::destroy(fontList_[i]);
    }
    fontList_.clear();
}

void gosRenderer::initRenderStates() {

	renderStates_[gos_State_Texture] = INVALID_TEXTURE_ID;
	renderStates_[gos_State_Texture2] = INVALID_TEXTURE_ID;
    renderStates_[gos_State_Texture3] = INVALID_TEXTURE_ID;
	renderStates_[gos_State_Filter] = gos_FilterNone;
	renderStates_[gos_State_ZCompare] = 1; 
    renderStates_[gos_State_ZWrite] = 1;
	renderStates_[gos_State_AlphaTest] = 0;
	renderStates_[gos_State_Perspective] = 1;
	renderStates_[gos_State_Specular] = 0;
	renderStates_[gos_State_Dither] = 0;
	renderStates_[gos_State_Clipping] = 0;	
	renderStates_[gos_State_WireframeMode] = 0;
	renderStates_[gos_State_AlphaMode] = gos_Alpha_OneZero;
	renderStates_[gos_State_TextureAddress] = gos_TextureWrap;
	renderStates_[gos_State_ShadeMode] = gos_ShadeGouraud;
	renderStates_[gos_State_TextureMapBlend] = gos_BlendModulateAlpha;
	renderStates_[gos_State_MipMapBias] = 0;
	renderStates_[gos_State_Fog]= 0;
	renderStates_[gos_State_MonoEnable] = 0;
	renderStates_[gos_State_Culling] = gos_Cull_None;
	renderStates_[gos_State_StencilEnable] = 0;
	renderStates_[gos_State_StencilFunc] = gos_Cmp_Never;
	renderStates_[gos_State_StencilRef] = 0;
	renderStates_[gos_State_StencilMask] = 0xffffffff;
	renderStates_[gos_State_StencilZFail] = gos_Stencil_Keep;
	renderStates_[gos_State_StencilFail] = gos_Stencil_Keep;
	renderStates_[gos_State_StencilPass] = gos_Stencil_Keep;
	renderStates_[gos_State_Multitexture] = gos_Multitexture_None;
	renderStates_[gos_State_Ambient] = 0xffffff;
	renderStates_[gos_State_Lighting] = 0;
	renderStates_[gos_State_NormalizeNormals] = 0;
	renderStates_[gos_State_VertexBlend] = 0;

    applyRenderStates();
    renderStatesStackPointer = -1;
}

void gosRenderer::pushRenderStates()
{
    gosASSERT(renderStatesStackPointer>=-1 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE - 1);
    if(!(renderStatesStackPointer>=-1 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE - 1)) {
        return;
    }

    renderStatesStackPointer++;
    memcpy(&statesStack_[renderStatesStackPointer], &renderStates_, sizeof(renderStates_));
}

void gosRenderer::popRenderStates()
{
    gosASSERT(renderStatesStackPointer>=0 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE);
    
    if(!(renderStatesStackPointer>=0 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE)) {
        return;
    }

    memcpy(&renderStates_, &statesStack_[renderStatesStackPointer], sizeof(renderStates_));
    renderStatesStackPointer--;
}

void gosRenderer::applyRenderStates() {

   ////////////////////////////////////////////////////////////////////////////////
   switch(renderStates_[gos_State_ZWrite]) {
       case 0: glDepthMask(GL_FALSE); break;
       case 1: glDepthMask(GL_TRUE); break;
       default: gosASSERT(0 && "Wrong depth write value");
   }
   curStates_[gos_State_ZWrite] = renderStates_[gos_State_ZWrite];

   ////////////////////////////////////////////////////////////////////////////////
   if(0 == renderStates_[gos_State_ZCompare]) {
       glDisable(GL_DEPTH_TEST);
   } else {
       glEnable(GL_DEPTH_TEST);
   }
   switch(renderStates_[gos_State_ZCompare]) {
       case 0: glDepthFunc(GL_ALWAYS); break;
       case 1: glDepthFunc(GL_LEQUAL); break;
       case 2: glDepthFunc(GL_LESS); break;
       default: gosASSERT(0 && "Wrong depth test value");
   }
   curStates_[gos_State_ZCompare] = renderStates_[gos_State_ZCompare];

   ////////////////////////////////////////////////////////////////////////////////
   bool disable_blending = renderStates_[gos_State_AlphaMode] == gos_Alpha_OneZero;
   if(disable_blending) {
       glDisable(GL_BLEND);
   } else {
       glEnable(GL_BLEND);
   }
   switch(renderStates_[gos_State_AlphaMode]) {
       case gos_Alpha_OneZero:          glBlendFunc(GL_ONE, GL_ZERO); break;
       case gos_Alpha_OneOne:           glBlendFunc(GL_ONE, GL_ONE); break;
       case gos_Alpha_AlphaInvAlpha:    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
       case gos_Alpha_OneInvAlpha:      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
       case gos_Alpha_AlphaOne:         glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
       default: gosASSERT(0 && "Wrong alpha mode value");
   }
   curStates_[gos_State_AlphaMode] = renderStates_[gos_State_AlphaMode];

   ////////////////////////////////////////////////////////////////////////////////
   bool enable_alpha_test = renderStates_[gos_State_AlphaTest] == 1;
   if(enable_alpha_test) {
       glEnable(GL_ALPHA_TEST);
       glAlphaFunc(GL_NOTEQUAL, 0.0f);
   } else {
       glDisable(GL_ALPHA_TEST);
   }
   curStates_[gos_State_AlphaTest] = renderStates_[gos_State_AlphaTest];

   ////////////////////////////////////////////////////////////////////////////////
   TexFilterMode filter = TFM_NONE;
   switch(renderStates_[gos_State_Filter]) {
       case gos_FilterNone: filter = TFM_NEAREST; break;
       case gos_FilterBiLinear : filter = TFM_LINEAR; break;
       case gos_FilterTriLinear: filter = TFM_LNEAR_MIPMAP_LINEAR; break;
   }
   // no mips for now, so ensure no invalid filters used
   //gosASSERT(filter == TFM_NEAREST || filter == TFM_LINEAR);
   // i do not know of any mipmaps that we are using
   if(filter == TFM_LNEAR_MIPMAP_LINEAR)
       filter = TFM_LINEAR;

   // in this case does not necessaily mean, that state was set, because in OpenGL this is binded to texture (unless separate sampler state extension is used, which is not currently)
   curStates_[gos_State_Filter] = renderStates_[gos_State_Filter];
  
   ////////////////////////////////////////////////////////////////////////////////
   TexAddressMode address_mode = 
       renderStates_[gos_State_TextureAddress] == gos_TextureWrap ? TAM_REPEAT : TAM_CLAMP;
   // in this case does not necessaily mean, that state was set, because in OpenGL this is binded to texture (unless separate sampler state extension is used, which is not currently)
   curStates_[gos_State_TextureAddress] = renderStates_[gos_State_TextureAddress];

   ////////////////////////////////////////////////////////////////////////////////
   uint32_t tex_states[] = { gos_State_Texture, gos_State_Texture2, gos_State_Texture3 };
   for(int i=0; i<sizeof(tex_states) / sizeof(tex_states[0]); ++i) {
       DWORD gosTextureHandle = renderStates_[tex_states[i]];

       glActiveTexture(GL_TEXTURE0 + i);

       gosTexture* tex = gosTextureHandle == INVALID_TEXTURE_ID ? 0 : this->getTexture(gosTextureHandle);
       if(tex) {
           glBindTexture(GL_TEXTURE_2D, tex->getTextureId());
           setSamplerParams(tex->getTextureType(), address_mode, filter);

           gosTextureInfo texinfo;
           tex->getTextureInfo(&texinfo);
           if(renderStates_[gos_State_TextureMapBlend] == gos_BlendDecal && texinfo.format_ == gos_Texture_Alpha)
           {
               PAUSE((""));
           }

       } else {
           glBindTexture(GL_TEXTURE_2D, 0);
       }
       curStates_[tex_states[i]] = gosTextureHandle;
   }

}

void gosRenderer::beginFrame()
{
    num_draw_calls_ = 0;
}

void gosRenderer::endFrame()
{
    if(pendingRequest) {

        width_ = reqWidth;
        height_ = reqHeight;

        // x = 1/w; x =2*x - 1;
        // y = 1/h; y= 1- y; y =2*y - 1;
        // z = z;
        projection_ = mat4(2.0f / (float)width_, 0, 0.0f, -1.0f,
                0, -2.0f / (float)height_, 0.0f, 1.0f,
                0, 0, 1.0f, 0.0f,
                0, 0, 0.0f, 1.0f);

        if(graphics::resize_window(win_h_, width_, height_)) {

            graphics::set_window_fullscreen(win_h_, reqGotoFullscreen);

            glViewport(0, 0, width_, height_);

            Environment.screenWidth = width_;
            Environment.screenHeight = height_;
        }
        pendingRequest = false;
    }
}

bool gosRenderer::beforeDrawCall()
{
    num_draw_calls_++;
    if(break_draw_call_num_ == num_draw_calls_ && break_on_draw_call_) {
        PAUSE(("Draw call %d break\n", num_draw_calls_ - 1));
    }

    return (num_draw_calls_ > num_draw_calls_to_draw_) && num_draw_calls_to_draw_ != 0;
}

void gosRenderer::afterDrawCall()
{
}

void gosRenderer::drawQuads(gos_VERTEX* vertices, int count) {
    gosASSERT(vertices);

    if(beforeDrawCall()) return;

    int num_quads = count / 4;
    int num_vertices = num_quads * 6;

    if(quads_->getNumVertices() + num_vertices > quads_->getVertexCapacity()) {
        applyRenderStates();
        gosShaderMaterial* mat = 
            curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;

        mat->setTransform(projection_);
        quads_->draw(mat);
        quads_->rewind();
    } 

    gosASSERT(quads_->getNumVertices() + num_vertices <= quads_->getVertexCapacity());
    for(int i=0; i<count;i+=4) {

        quads_->addVertices(vertices + 4*i + 0, 1);
        quads_->addVertices(vertices + 4*i + 1, 1);
        quads_->addVertices(vertices + 4*i + 2, 1);

        quads_->addVertices(vertices + 4*i + 0, 1);
        quads_->addVertices(vertices + 4*i + 2, 1);
        quads_->addVertices(vertices + 4*i + 3, 1);
    }

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();

    gosShaderMaterial* mat = 
        curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;
    mat->setTransform(projection_);
    quads_->draw(mat);
    quads_->rewind();

    afterDrawCall();
}

void gosRenderer::drawLines(gos_VERTEX* vertices, int count) {
    gosASSERT(vertices);

    if(beforeDrawCall()) return;

    if(lines_->getNumVertices() + count > lines_->getVertexCapacity()) {
        applyRenderStates();
        basic_material_->setTransform(projection_);
        lines_->draw(basic_material_);
        lines_->rewind();
    }

    gosASSERT(lines_->getNumVertices() + count <= lines_->getVertexCapacity());
    lines_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    basic_material_->setTransform(projection_);
    lines_->draw(basic_material_);
    lines_->rewind();

    afterDrawCall();
}

void gosRenderer::drawPoints(gos_VERTEX* vertices, int count) {
    gosASSERT(vertices);

    if(beforeDrawCall()) return;

    if(points_->getNumVertices() + count > points_->getVertexCapacity()) {
        applyRenderStates();
        basic_material_->setTransform(projection_);
        points_->draw(basic_material_);
        points_->rewind();
    } 

    gosASSERT(points_->getNumVertices() + count <= points_->getVertexCapacity());
    points_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    points_->draw(basic_material_);
    points_->rewind();

    afterDrawCall();
}

void gosRenderer::drawTris(gos_VERTEX* vertices, int count) {
    gosASSERT(vertices);

    gosASSERT((count % 3) == 0);

    if(beforeDrawCall()) return;

    if(tris_->getNumVertices() + count > tris_->getVertexCapacity()) {
        applyRenderStates();
        gosShaderMaterial* mat = 
            curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;
        mat->setTransform(projection_);
        tris_->draw(mat);
        tris_->rewind();
    } 

    gosASSERT(tris_->getNumVertices() + count <= tris_->getVertexCapacity());
    tris_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    gosShaderMaterial* mat = 
        curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;
    mat->setTransform(projection_);
    tris_->draw(mat);
    tris_->rewind();

    afterDrawCall();
}

void gosRenderer::drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices) {
    gosASSERT(vertices && indices);

    gosASSERT((num_indices % 3) == 0);

    if(beforeDrawCall()) return;

    bool not_enough_vertices = indexed_tris_->getNumVertices() + num_vertices > indexed_tris_->getVertexCapacity();
    bool not_enough_indices = indexed_tris_->getNumIndices() + num_indices > indexed_tris_->getIndexCapacity();
    if(not_enough_vertices || not_enough_indices){
        applyRenderStates();
        gosShaderMaterial* mat = 
            curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;
        mat->setTransform(projection_);
        indexed_tris_->drawIndexed(mat);
        indexed_tris_->rewind();
    } 

    gosASSERT(indexed_tris_->getNumVertices() + num_vertices <= indexed_tris_->getVertexCapacity());
    gosASSERT(indexed_tris_->getNumIndices() + num_indices <= indexed_tris_->getIndexCapacity());
    indexed_tris_->addVertices(vertices, num_vertices);
    indexed_tris_->addIndices(indices, num_indices);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    gosShaderMaterial* mat = 
        curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;
    mat->setTransform(projection_);
    indexed_tris_->drawIndexed(mat);
    indexed_tris_->rewind();

    afterDrawCall();
}

static int get_next_break(const char* text) {
    const char* start = text;
    do {
        char c = *text;
        if(c==' ' || c=='\n')
            return text - start;
    } while(*text++);

    return text - start - 1;
}

int findTextBreak(const char* text, const int count, const gosFont* font, const int region_width, int* out_str_width) {

    int width = 0;
    int pos = 0;

    int space_adv = font->getCharAdvance(' ');

    while(text[pos]) {

        int break_pos = get_next_break(text + pos);

        int cur_width = 0;
        for(int j=0;j<break_pos;++j) {
            cur_width += font->getCharAdvance(text[pos + j]);
        }

        // if next possible break will not fit, then return now
        if(width + cur_width >= region_width) {

            if(pos == 0) { // handle case when only one word in line and it does not fit it, just return whole line
                width = cur_width;
                pos = break_pos;
            }
            break;
        } else {
            width += cur_width;
            pos += break_pos;

            if(text[pos] == '\n') {
                pos++;
                break;
            }

            if(text[pos] == ' ') {
                width += space_adv;
                pos++;
            }
        }
    }

    if(out_str_width)
        *out_str_width = width;
    return pos;
}
// returnes num lines in text which should be wrapped in region_width
int calcTextHeight(const char* text, const int count, const gosFont* font, int region_width)
{
    int pos = 0;
    int num_lines = 0;
    while(pos < count) {

        int num_chars = findTextBreak(text + pos, count - pos, font, region_width, NULL);
        pos += num_chars;
        num_lines++;
    }
    return num_lines;
}

void addCharacter(gosMesh* text_, const float u, const float v, const float char_du, const float char_dv, const int x, const int y, const int char_w, const int char_h) {

    gos_VERTEX tr, tl, br, bl;

    tl.x = x;
    tl.y = y;
    tl.z = 0;
    tl.u = u;
    tl.v = v;
    tl.argb = 0xffffffff;

    tr.x = x + char_w;
    tr.y = y;
    tr.z = 0;
    tr.u = u + char_du;
    tr.v = v;
    tr.argb = 0xffffffff;

    bl.x = x;
    bl.y = y + char_h;
    bl.z = 0;
    bl.u = u;
    bl.v = v + char_dv;
    bl.argb = 0xffffffff;

    br.x = x + char_w;
    br.y = y + char_h;
    br.z = 0;
    br.u = u + char_du;
    br.v = v + char_dv;
    br.argb = 0xffffffff;

    text_->addVertices(&tl, 1);
    text_->addVertices(&tr, 1);
    text_->addVertices(&bl, 1);

    text_->addVertices(&tr, 1);
    text_->addVertices(&br, 1);
    text_->addVertices(&bl, 1);

}

void gosRenderer::drawText(const char* text) {
    gosASSERT(text);

    if(beforeDrawCall()) return;

    const int count = (int)strlen(text);  
/*
    if(text_->getNumVertices() + count > text_->getCapacity()) {
        applyRenderStates();
        gosShaderMaterial* mat = 
            curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;
        text_->draw(mat);
        text_->rewind();
    } 
*/

    // TODO: take text region into account!!!!
    
    gosASSERT(text_->getNumVertices() + 6 * count <= text_->getVertexCapacity());

    int x, y;
    getTextPos(x, y);
    const int start_x = x;

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;
    const int char_w = font->getMaxCharWidth();
    const int char_h = font->getMaxCharHeight();


    const DWORD tex_id = font->getTextureId();
    const gosTexture* tex = getTexture(tex_id);
    gosTextureInfo ti;
    tex->getTextureInfo(&ti);
    const float tex_width = (float)ti.width_;
    const float tex_height = (float)ti.height_;
    
    const float char_du = (float)char_w / tex_width;
    const float char_dv = (float)char_h / tex_height;

    const int font_height = font->getMaxCharHeight();

    const int region_width = getTextRegionWidth();
    const int region_height = getTextRegionHeight();

    const int num_lines = calcTextHeight(text, count, font, region_width);
    if(ta.WrapType == 3) { // center in Y direction as well
        y += (region_height - num_lines * font_height) / 2;
    }
   
    int pos = 0;
    int str_width = 0;
    while(pos < count) {

        x = start_x;    
        int num_chars = findTextBreak(text + pos, count - pos, font, region_width, &str_width);

        // WrapType		- 0=Left aligned, 1=Right aligned, 2=Centered, 3=Centered in region (X and Y)
        switch(ta.WrapType) {
            case 0: break;
            case 1: x += region_width - str_width; break;
            case 2: x += (region_width - str_width) / 2; break;
            case 3: //ehh... handle later, must know number of lines
                    // for now only center in X
                    x += (region_width - str_width) / 2;
                    break;
        }

        for(int i=0; i<num_chars; ++i) {

            const char c = text[i + pos];
            int advance;
            uint32_t iu, iv;
            font->getCharUV(c, &iu, &iv);
            advance = font->getCharAdvance(c);
            float u = (float)iu / tex_width;
            float v = (float)iv / tex_height;
            addCharacter(text_, u, v, char_du, char_dv, x, y, char_w, char_h);

            x += advance;
        }
        y += font_height;
        pos += num_chars;
    }
    // FIXME: save states before messing with it, because user code can set its ow and does not know that something was changed by us
    
    int prev_texture = getRenderState(gos_State_Texture);
    
    // All states are set by client code
    // so we only set font texture
    setRenderState(gos_State_Texture, tex_id);
    setRenderState(gos_State_Filter, gos_FilterNone);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    gosShaderMaterial* mat = text_material_;

    //ta.Foreground
    vec4 fg;
    fg.x = (ta.Foreground & 0xFF0000) >> 16;
    fg.y = (ta.Foreground & 0xFF00) >> 8;
    fg.z = ta.Foreground & 0xFF;
    fg.w = 255;//(ta.Foreground & 0xFF000000) >> 24;
    fg = fg / 255.0f; 
    mat->getShader()->setFloat4("Foreground", fg);
    //ta.Size 
    //ta.WordWrap 
    //ta.Proportional
    //ta.Bold
    //ta.Italic
    //ta.WrapType
    //ta.DisableEmbeddedCodes

    mat->setTransform(projection_);
    text_->draw(mat);
    text_->rewind();

    setRenderState(gos_State_Texture, prev_texture);

    afterDrawCall();
}

void gosRenderer::flush()
{
}

void gos_CreateRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h) {

    g_gos_renderer = new gosRenderer(ctx_h, win_h, w, h);
    g_gos_renderer->init();
}

void gos_DestroyRenderer() {

    g_gos_renderer->destroy();
    delete g_gos_renderer;
}

void gos_RendererBeginFrame() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->beginFrame();
}

void gos_RendererEndFrame() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->endFrame();
}

////////////////////////////////////////////////////////////////////////////////
gosFont* gosFont::load(const char* fontFile) {

    char fname[256];
    char dir[256];
    _splitpath(fontFile, NULL, dir, fname, NULL);
    const char* tex_ext = ".bmp";
    const char* glyph_ext = ".glyph";

    const uint32_t textureNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(tex_ext) + 1;
    char* textureName = new char[textureNameSize];

    const uint32_t glyphNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(glyph_ext) + 1;
    char* glyphName = new char[glyphNameSize];
    snprintf(textureName, textureNameSize, "%s/%s%s", dir, fname, tex_ext);
    snprintf(glyphName, glyphNameSize, "%s/%s%s", dir, fname, glyph_ext);

    gosTexture* ptex = new gosTexture(gos_Texture_Alpha, textureName, 0, NULL, 0, false);
    if(!ptex || !ptex->createHardwareTexture()) {
        STOP(("Failed to create font texture: %s\n", textureName));
    }

    DWORD tex_id = getGosRenderer()->addTexture(ptex);

    gosFont* font = new gosFont();
    if(!gos_load_glyphs(glyphName, font->gi_)) {
        delete font;
        STOP(("Failed to load font glyphs: %s\n", glyphName));
        return NULL;
    }

    font->font_name_ = new char[strlen(fname) + 1];
    strcpy(font->font_name_, fname);
    font->tex_id_ = tex_id;

    delete[] textureName;
    delete[] glyphName;

    return font;

}

void gosFont::destroy(gosFont* font) {
    delete font;
}

void gosFont::getCharUV(int c, uint32_t* u, uint32_t* v) const {

    gosASSERT(u && v);

    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= gi_.num_glyphs_) {
        *u = *v = 0;
        return;
    }

    *u = gi_.glyphs_[pos].u;
    *v = gi_.glyphs_[pos].v;
}

int gosFont::getCharAdvance(int c) const
{
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= gi_.num_glyphs_) {
        return getMaxCharWidth();
    }

    return gi_.glyphs_[pos].advance;
}


////////////////////////////////////////////////////////////////////////////////
// graphics
//
void _stdcall gos_DrawLines(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawLines(Vertices, NumVertices);
}
void _stdcall gos_DrawPoints(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawPoints(Vertices, NumVertices);
}

bool g_disable_quads = true;
void _stdcall gos_DrawQuads(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(Vertices, NumVertices);
}
void _stdcall gos_DrawTriangles(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawTris(Vertices, NumVertices);
}

void __stdcall gos_GetViewport( float* pViewportMulX, float* pViewportMulY, float* pViewportAddX, float* pViewportAddY )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->getViewportTransform(pViewportMulX, pViewportMulY, pViewportAddX, pViewportAddY);
}

HGOSFONT3D __stdcall gos_LoadFont( const char* FontFile, DWORD StartLine/* = 0*/, int CharCount/* = 256*/, DWORD TextureHandle/*=0*/)
{
    gosFont* font = gosFont::load(FontFile);
    getGosRenderer()->addFont(font);
    return font;
}

void __stdcall gos_DeleteFont( HGOSFONT3D FontHandle )
{
    gosASSERT(FontHandle);
    gosFont* font = FontHandle;
    getGosRenderer()->deleteFont(font);
}

DWORD __stdcall gos_NewEmptyTexture( gos_TextureFormat Format, const char* Name, DWORD HeightWidth, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    int w = HeightWidth;
    int h = HeightWidth;
    if(HeightWidth&0xffff0000)
    {
        h = HeightWidth >> 16;
        w = HeightWidth & 0xffff;
    }
    gosTexture* ptex = new gosTexture(Format, Hints, w, h, Name);

    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}
DWORD __stdcall gos_NewTextureFromMemory( gos_TextureFormat Format, const char* FileName, BYTE* pBitmap, DWORD Size, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    gosASSERT(pFunc == 0);

    gosTexture* ptex = new gosTexture(Format, FileName, Hints, pBitmap, Size, true);
    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}

DWORD __stdcall gos_NewTextureFromFile( gos_TextureFormat Format, const char* FileName, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    gosTexture* ptex = new gosTexture(Format, FileName, Hints, NULL, 0, false);
    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }
    return g_gos_renderer->addTexture(ptex);
}
void __stdcall gos_DestroyTexture( DWORD Handle )
{
    g_gos_renderer->deleteTexture(Handle);
}

void __stdcall gos_LockTexture( DWORD Handle, DWORD MipMapSize, bool ReadOnly, TEXTUREPTR* TextureInfo )
{
    // TODO: does not really locks texture
    
    // not implemented yet
    gosASSERT(MipMapSize == 0);
    int mip_level = 0; //func(MipMapSize);

    gosTextureInfo info;
    int pitch = 0;
    gosTexture* ptex = g_gos_renderer->getTexture(Handle);
    ptex->getTextureInfo(&info);
    BYTE* pdata = ptex->Lock(mip_level, ReadOnly, &pitch);

    TextureInfo->pTexture = (DWORD*)pdata;
    TextureInfo->Width = info.width_;
    TextureInfo->Height = info.height_;
    TextureInfo->Pitch = pitch;
    TextureInfo->Type = info.format_;

    //gosASSERT(0 && "Not implemented");
}

void __stdcall gos_UnLockTexture( DWORD Handle )
{
    gosTexture* ptex = g_gos_renderer->getTexture(Handle);
    ptex->Unlock();

    //gosASSERT(0 && "Not implemented");
}

void __stdcall gos_PushRenderStates()
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->pushRenderStates();
} 

void __stdcall gos_PopRenderStates()
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->popRenderStates();
}

void __stdcall gos_RenderIndexedArray( gos_VERTEX* pVertexArray, DWORD NumberVertices, WORD* lpwIndices, DWORD NumberIndices )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(pVertexArray, NumberVertices, lpwIndices, NumberIndices);
}

void __stdcall gos_RenderIndexedArray( gos_VERTEX_2UV* pVertexArray, DWORD NumberVertices, WORD* lpwIndices, DWORD NumberIndices )
{
   gosASSERT(0 && "not implemented");
}

void __stdcall gos_SetRenderState( gos_RenderState RenderState, int Value )
{
    gosASSERT(g_gos_renderer);
    // gos_BlendDecal mode is not suported (currently texture color always modulated with vertex color)
    //gosASSERT(RenderState!=gos_State_TextureMapBlend || (Value == gos_BlendDecal));
    g_gos_renderer->setRenderState(RenderState, Value);
}

void __stdcall gos_SetScreenMode( DWORD Width, DWORD Height, DWORD bitDepth/*=16*/, DWORD Device/*=0*/, bool disableZBuffer/*=0*/, bool AntiAlias/*=0*/, bool RenderToVram/*=0*/, bool GotoFullScreen/*=0*/, int DirtyRectangle/*=0*/, bool GotoWindowMode/*=0*/, bool EnableStencil/*=0*/, DWORD Renderer/*=0*/)
{
    gosASSERT(g_gos_renderer);
    gosASSERT((GotoFullScreen && !GotoWindowMode) || (!GotoFullScreen&&GotoWindowMode) || (!GotoFullScreen&&!GotoWindowMode));

    g_gos_renderer->setScreenMode(Width, Height, bitDepth, GotoFullScreen, AntiAlias);
}

void __stdcall gos_SetupViewport( bool FillZ, float ZBuffer, bool FillBG, DWORD BGColor, float top, float left, float bottom, float right, bool ClearStencil/*=0*/, DWORD StencilValue/*=0*/)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setupViewport(FillZ, ZBuffer, FillBG, BGColor, top, left, bottom, right, ClearStencil, StencilValue);
}

void __stdcall gos_TextDraw( const char *Message, ... )
{

	if (!Message || !strlen(Message)) {
        SPEW(("GRAPHICS", "Trying to draw zero legth string\n"));
        return;
    }

	va_list	ap;
    va_start(ap, Message);

    static const int MAX_TEXT_LEN = 4096;
	char text[MAX_TEXT_LEN] = {0};

	vsnprintf(text, MAX_TEXT_LEN - 1, Message, ap);

	size_t len = strlen(text);
	text[len] = '\0';

    va_end(ap);

    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawText(text);
}

void __stdcall gos_TextDrawBackground( int Left, int Top, int Right, int Bottom, DWORD Color )
{
    // TODO: Is it correctly Implemented?
    gosASSERT(g_gos_renderer);

    //PAUSE((""));

    gos_VERTEX v[4];
    v[0].x = Left;
    v[0].y = Top;
    v[0].z = 0;
	v[0].argb = Color;
	v[0].frgb = 0;
	v[0].u = 0;	
	v[0].v = 0;	
    memcpy(&v[1], &v[0], sizeof(gos_VERTEX));
    memcpy(&v[2], &v[0], sizeof(gos_VERTEX));
    memcpy(&v[3], &v[0], sizeof(gos_VERTEX));
    v[1].x = Right;
    v[1].u = 1.0f;

    v[2].x = Right;
    v[2].y = Bottom;
    v[2].u = 1.0f;
    v[2].v = 0.0f;

    v[1].y = Bottom;
    v[1].v = 1.0f;

    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(v, 4);
}

void __stdcall gos_TextSetAttributes( HGOSFONT3D FontHandle, DWORD Foreground, float Size, bool WordWrap, bool Proportional, bool Bold, bool Italic, DWORD WrapType/*=0*/, bool DisableEmbeddedCodes/*=0*/)
{
    gosASSERT(g_gos_renderer);

    gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    ta.FontHandle = FontHandle;
    ta.Foreground = Foreground;
    ta.Size = Size;
    ta.WordWrap = WordWrap;
    ta.Proportional = Proportional;
    ta.Bold = Bold;
    ta.Italic = Italic;
    ta.WrapType = WrapType;
    ta.DisableEmbeddedCodes = DisableEmbeddedCodes;
}

void __stdcall gos_TextSetPosition( int XPosition, int YPosition )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setTextPos(XPosition, YPosition);
}

void __stdcall gos_TextSetRegion( int Left, int Top, int Right, int Bottom )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setTextRegion(Left, Top, Right, Bottom);
}

void __stdcall gos_TextStringLength( DWORD* Width, DWORD* Height, const char *fmt, ... )
{
    gosASSERT(Width && Height);

    if(!fmt) {
        SPEW(("GRAPHICS", "No text to calculate length!"));
        *Width = 1;
        *Height = 1;
        return;
    }

    const int   MAX_TEXT_LEN = 4096;
	char        text[MAX_TEXT_LEN] = {0};
	va_list	    ap;

    va_start(ap, fmt);
	vsnprintf(text, MAX_TEXT_LEN - 1, fmt, ap);
    va_end(ap);

	size_t len = strlen(text);
    text[len] = '\0';

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;
    gosASSERT(font);

    int num_newlines = 0;
    int max_width = 0;
    int cur_width = 0;
    const char* txtptr = text;

    while(*txtptr) {
        if(*txtptr++ == '\n') {
            num_newlines++;
            max_width = max_width > cur_width ? max_width : cur_width;
            cur_width = 0;
        } else {
            const int cw = font->getCharAdvance(*txtptr);
            cur_width += cw;
        }
    }
    max_width = max_width > cur_width ? max_width : cur_width;

    *Width = max_width;
    *Height = (num_newlines + 1) * font->getMaxCharHeight();
}

////////////////////////////////////////////////////////////////////////////////
size_t __stdcall gos_GetMachineInformation( MachineInfo mi, int Param1/*=0*/, int Param2/*=0*/, int Param3/*=0*/, int Param4/*=0*/)
{
    // TODO:
    if(mi == gos_Info_GetDeviceLocalMemory)
        return 1024*1024*1024;
    if(mi == gos_Info_GetDeviceAGPMemory)
        return 512*1024*1024; 
    if (mi == gos_Info_CanMultitextureDetail)
        return true;
    if(mi == gos_Info_NumberDevices)
        return 1;
    if(mi == gos_Info_GetDeviceName)
        return (size_t)glGetString(GL_RENDERER);
    if(mi == gos_Info_ValidMode) {
        int xres = Param2;
        int yres = Param3;
        int bpp = Param4;
        return graphics::is_mode_supported(xres, yres, bpp) ? 1 : 0;
    }
    return 0;
}

int gos_GetWindowDisplayIndex()
{   
    gosASSERT(g_gos_renderer);
    
    return graphics::get_window_display_index(g_gos_renderer->getRenderContextHandle());
}

int gos_GetNumDisplayModes(int DisplayIndex)
{
    return graphics::get_num_display_modes(DisplayIndex);
}

bool gos_GetDisplayModeByIndex(int DisplayIndex, int ModeIndex, int* XRes, int* YRes, int* BitDepth)
{
    return graphics::get_display_mode_by_index(DisplayIndex, ModeIndex, XRes, YRes, BitDepth);
}

#include "gameos_graphics_debug.cpp"
