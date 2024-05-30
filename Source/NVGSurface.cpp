/*
 // Copyright (c) 2021-2022 Timothy Schoen and Alex Mitchell
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
using namespace juce::gl;

#include <nanovg.h>
#ifdef NANOVG_GL_IMPLEMENTATION
#include <nanovg_gl.h>
#include <nanovg_gl_utils.h>
#endif

#include "NVGSurface.h"

#include "PluginEditor.h"
#include "PluginMode.h"
#include "Canvas.h"
#include "Tabbar/WelcomePanel.h"
#include "Sidebar/Sidebar.h" // meh...

#define ENABLE_FPS_COUNT 0

class FrameTimer
{
public:
    FrameTimer()
    {
        startTime = getNow();
        prevTime = startTime;
    }
    
    void render(NVGcontext* nvg)
    {
        nvgBeginPath(nvg);
        nvgRect(nvg, 0, 0, 40, 22);
        nvgFillColor(nvg, nvgRGBA(40, 40, 40, 255));
        nvgFill(nvg);
        
        nvgFontSize(nvg, 20.0f);
        nvgTextAlign(nvg,NVG_ALIGN_LEFT|NVG_ALIGN_TOP);
        nvgFillColor(nvg, nvgRGBA(240,240,240,255));
        char fpsBuf[16];
        snprintf(fpsBuf, 16, "%d", static_cast<int>(round(1.0f / getAverageFrameTime())));
        nvgText(nvg, 7, 2, fpsBuf, nullptr);
    }
    void addFrameTime()
    {
        auto timeSeconds = getTime();
        auto dt = timeSeconds - prevTime;
        perf_head = (perf_head+1) % 32;
        frame_times[perf_head] = dt;
        prevTime = timeSeconds;
    }
    
    double getTime() { return getNow() - startTime; }
private:
    double getNow()
    {
        auto ticks = Time::getHighResolutionTicks();
        return Time::highResolutionTicksToSeconds(ticks);
    }
    
    float getAverageFrameTime()
    {
        float avg = 0;
        for (int i = 0; i < 32; i++) {
            avg += frame_times[i];
        }
        return avg / (float)32;
    }
    
    float frame_times[32] = {};
    int perf_head = 0;
    double startTime = 0, prevTime = 0;
};


NVGSurface::NVGSurface(PluginEditor* e) : editor(e)
{
#ifdef NANOVG_GL_IMPLEMENTATION
    glContext = std::make_unique<OpenGLContext>();
    glContext->setOpenGLVersionRequired(OpenGLContext::OpenGLVersion::openGL3_2);
    glContext->setMultisamplingEnabled(false);
    glContext->setSwapInterval(0);
#endif
    
#if ENABLE_FPS_COUNT
    frameTimer = std::make_unique<FrameTimer>();
#endif
    
    setInterceptsMouseClicks(false, false);
    setWantsKeyboardFocus(false);
    
    setSize(1,1);
    
    // Start rendering asynchronously, so we are sure the window has been added to the desktop
    // kind of a hack, but works well enough
    MessageManager::callAsync([_this = SafePointer(this)](){
        if(_this) {
            _this->initialise();
            _this->updateBufferSize();
            
            // Render on vblank
            _this->vBlankAttachment = std::make_unique<VBlankAttachment>(_this.getComponent(), [_this](){
                if(_this) {
                    _this->editor->pd->messageDispatcher->dequeueMessages();
                    _this->render();
                }
            });
        }
    });
}

NVGSurface::~NVGSurface()
{
    detachContext();
}

void NVGSurface::initialise()
{
#ifdef NANOVG_METAL_IMPLEMENTATION
    auto renderScale = getRenderScale();
    auto* peer = getPeer()->getNativeHandle();
    auto* view = OSUtils::MTLCreateView(peer, 0, 0, getWidth(), getHeight());
    setView(view);
    nvg = nvgCreateContext(view, NVG_ANTIALIAS | NVG_TRIPLE_BUFFER, getWidth() * renderScale, getHeight() * renderScale);
    setVisible(true);
#if JUCE_IOS
    resized();
#endif
#else
    setVisible(true);
    glContext->attachTo(*this);
    glContext->initialiseOnThread();
    glContext->makeActive();
    nvg = nvgCreateContext(NVG_ANTIALIAS);
#endif
    
    invalidateAll();
    
    if (!nvg) std::cerr << "could not initialise nvg" << std::endl;
    nvgCreateFontMem(nvg, "Inter", (unsigned char*)BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize, 0);
    nvgCreateFontMem(nvg, "Inter-Regular", (unsigned char*)BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize, 0);
    nvgCreateFontMem(nvg, "Inter-Bold", (unsigned char*)BinaryData::InterBold_ttf, BinaryData::InterBold_ttfSize, 0);
    nvgCreateFontMem(nvg, "Inter-SemiBold", (unsigned char*)BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize, 0);
    nvgCreateFontMem(nvg, "Inter-Tabular", (unsigned char*)BinaryData::InterTabular_ttf, BinaryData::InterTabular_ttfSize, 0);
    nvgCreateFontMem(nvg, "icon_font-Regular", (unsigned char*)BinaryData::IconFont_ttf, BinaryData::IconFont_ttfSize, 0);
}

void NVGSurface::updateBufferSize()
{
    float pixelScale = getRenderScale();
    int scaledWidth = getWidth() * pixelScale;
    int scaledHeight = getHeight() * pixelScale;
    
    if(fbWidth != scaledWidth || fbHeight != scaledHeight || !mainFBO) {
        if(invalidFBO) nvgDeleteFramebuffer(invalidFBO);
        if(mainFBO) nvgDeleteFramebuffer(mainFBO);
        mainFBO = nvgCreateFramebuffer(nvg, scaledWidth, scaledHeight, NVG_IMAGE_PREMULTIPLIED);
        invalidFBO = nvgCreateFramebuffer(nvg, scaledWidth, scaledHeight, NVG_IMAGE_PREMULTIPLIED);
        fbWidth = scaledWidth;
        fbHeight = scaledHeight;
        invalidArea = getLocalBounds();
        lastScaleFactor = pixelScale;
    }
    
    //scaleChanged = !approximatelyEqual(lastScaleFactor, pixelScale);
}


#ifdef NANOVG_GL_IMPLEMENTATION
void NVGSurface::timerCallback()
{
    updateBounds(newBounds);
    if (getBounds() == newBounds)
        stopTimer();
}
#endif


void NVGSurface::triggerRepaint()
{
    needsBufferSwap = true;
}

bool NVGSurface::makeContextActive()
{
#ifdef NANOVG_METAL_IMPLEMENTATION
    // No need to make context active with Metal, so just check if we have initialised and return that
    return isAttached();
#else
    if(glContext) return glContext->makeActive();
#endif
    
    return false;
}

void NVGSurface::detachContext()
{
#ifdef NANOVG_METAL_IMPLEMENTATION
    if(auto* view = getView()) {
        OSUtils::MTLDeleteView(view);
        setView(nullptr);
    }
#else
    if(glContext) glContext->detach();
#endif
}


void NVGSurface::propertyChanged(String const& name, var const& value) {
    if(name == "global_scale")
    {
        // TODO: handle this?
        //sendContextDeleteMessage();
    }
}

float NVGSurface::getRenderScale() const
{
    auto desktopScale = Desktop::getInstance().getGlobalScaleFactor();
#ifdef NANOVG_METAL_IMPLEMENTATION
    if(!isAttached()) return 2.0f * desktopScale;
    return OSUtils::MTLGetPixelScale(getView()) * desktopScale;
#else
    if(!isAttached()) return desktopScale;
    return glContext->getRenderingScale();// * desktopScale;
#endif
}

void NVGSurface::updateBounds(Rectangle<int> bounds)
{
#ifdef NANOVG_GL_IMPLEMENTATION
    newBounds = bounds;
    if (hresize)
        setBounds(bounds.withHeight(getHeight()));
    else
        setBounds(bounds.withWidth(getWidth()));
    
    resizing = true;
#else
    setBounds(bounds);
#endif
}

void NVGSurface::resized()
{
#ifdef NANOVG_METAL_IMPLEMENTATION
    if(auto* view = getView()) {
        auto desktopScale = Desktop::getInstance().getGlobalScaleFactor();
        auto renderScale = OSUtils::MTLGetPixelScale(view); // TODO: we can simplify with getRenderScale() function, but needs testing on iOS
        auto* topLevel = getTopLevelComponent();
        auto bounds = topLevel->getLocalArea(this, getLocalBounds()).toFloat() * desktopScale;
        mnvgSetViewBounds(view, (renderScale * bounds.getWidth()), (renderScale * bounds.getHeight()));
    }
#endif
}

bool NVGSurface::isAttached() const
{
#ifdef NANOVG_METAL_IMPLEMENTATION
    return getView() != nullptr && nvg != nullptr;
#else
    return glContext->isAttached() && nvg != nullptr;
#endif
}


void NVGSurface::invalidateAll()
{
    invalidArea = getLocalBounds();
}

void NVGSurface::invalidateArea(Rectangle<int> area)
{
    invalidArea = invalidArea.getUnion(area);
}

void NVGSurface::render()
{
    auto startTime = Time::getMillisecondCounter();
    
    if(!isAttached() && isVisible()) initialise();
    
    bool hasCanvas = false;
    for(auto* split : editor->splitView.splits)
    {
        if(auto* cnv = split->getTabComponent()->getCurrentCanvas())
        {
            hasCanvas = true;
        }
    }
    // Manage showing/hiding welcome panel
    if(hasCanvas && editor->welcomePanel->isVisible()) {
        editor->welcomePanel->hide();
        editor->resized();
    }
    else if(!hasCanvas && !editor->welcomePanel->isVisible()) {
        editor->welcomePanel->show();
        editor->resized();
    }
    
    updateBufferSize();

    auto pixelScale = getRenderScale();
    if(!invalidArea.isEmpty() && makeContextActive()) {
        auto invalidated = invalidArea.expanded(1);
        
        // First, draw only the invalidated region to a separate framebuffer
        // I've found that nvgScissor doesn't always clip everything, meaning that there will be graphical glitches if we don't do this
        nvgBindFramebuffer(invalidFBO);
        nvgViewport(0, 0, getWidth() * pixelScale, getHeight() * pixelScale);
        nvgClear(nvg);
        
        nvgBeginFrame(nvg, getWidth(), getHeight(), pixelScale);
        nvgScissor (nvg, invalidated.getX(), invalidated.getY(), invalidated.getWidth(), invalidated.getHeight());

        editor->renderArea(nvg, invalidated);
        nvgEndFrame(nvg);
        
        nvgBindFramebuffer(mainFBO);
        nvgViewport(0, 0, getWidth() * pixelScale, getHeight() * pixelScale);
        nvgBeginFrame(nvg, getWidth(), getHeight(), pixelScale);
        nvgBeginPath(nvg);
        nvgRect(nvg, invalidated.getX(), invalidated.getY(), invalidated.getWidth(), invalidated.getHeight());
        nvgScissor(nvg, invalidated.getX(), invalidated.getY(), invalidated.getWidth(), invalidated.getHeight());
        nvgFillPaint(nvg, nvgImagePattern(nvg, 0, 0, getWidth(), getHeight(), 0, invalidFBO->image, 1));
        nvgFill(nvg);
        
#if ENABLE_FB_DEBUGGING
        static Random rng;
        nvgBeginPath(nvg);
        nvgFillColor(nvg, nvgRGBA(rng.nextInt(255), rng.nextInt(255), rng.nextInt(255), 0x50));
        nvgRect(nvg, 0, 0, getWidth(), getHeight());
        nvgFill(nvg);
#endif
        
        nvgEndFrame(nvg);
        
        nvgBindFramebuffer(nullptr);
        needsBufferSwap = true;
        invalidArea = Rectangle<int>(0, 0, 0, 0);
        
#if ENABLE_FPS_COUNT
        frameTimer->addFrameTime();
#endif
    }
    
    if(needsBufferSwap && makeContextActive()) {
        float pixelScale = getRenderScale();
        nvgViewport(0, 0, getWidth() * pixelScale, getHeight() * pixelScale);
        
        nvgBeginFrame(nvg, getWidth(), getHeight(), pixelScale);
        
        nvgBeginPath(nvg);
        nvgSave(nvg);
        nvgRect(nvg, 0, 0, getWidth(), getHeight());
        nvgScissor(nvg, 0, 0, getWidth(), getHeight());
        nvgFillPaint(nvg, nvgImagePattern(nvg, 0, 0, getWidth(), getHeight(), 0, mainFBO->image, 1));
        nvgFill(nvg);
        nvgRestore(nvg);
        
        if(!editor->pluginMode) {
            editor->splitView.render(nvg); // Render split view outlines and tab dnd areas
        }
        
#if ENABLE_FPS_COUNT
        nvgSave(nvg);
        frameTimer->render(nvg);
        nvgRestore(nvg);
#endif
        
        nvgEndFrame(nvg);
        
#ifdef NANOVG_GL_IMPLEMENTATION
        glContext->swapBuffers();
        if (resizing) {
            hresize = !hresize;
            resizing = false;
        }
        if (getBounds() != newBounds)
            startTimerHz(60);
#endif
        needsBufferSwap = false;
    }
    
    auto elapsed = Time::getMillisecondCounter() - startTime;
    // We update frambuffers after we call swapBuffers to make sure the frame is on time
    if(elapsed < 14) {
        for(auto* split : editor->splitView.splits)
        {
            if(auto* cnv = split->getTabComponent()->getCurrentCanvas())
            {
                cnv->updateFramebuffers(nvg, cnv->getLocalBounds(), 14 - elapsed);
            }
        }
    }
}
