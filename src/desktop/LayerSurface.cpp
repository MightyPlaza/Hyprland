#include "LayerSurface.hpp"
#include "../Compositor.hpp"
#include "../events/Events.hpp"
#include "../protocols/LayerShell.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../managers/SeatManager.hpp"

PHLLS CLayerSurface::create(SP<CLayerShellResource> resource) {
    PHLLS     pLS = SP<CLayerSurface>(new CLayerSurface(resource));

    CMonitor* pMonitor = resource->monitor.empty() ? g_pCompositor->getMonitorFromCursor() : g_pCompositor->getMonitorFromName(resource->monitor);

    pLS->surface->assign(resource->surface.lock(), pLS);

    if (!pMonitor) {
        Debug::log(ERR, "New LS has no monitor??");
        return pLS;
    }

    if (pMonitor->pMirrorOf)
        pMonitor = g_pCompositor->m_vMonitors.front().get();

    pLS->self = pLS;

    pLS->szNamespace = resource->layerNamespace;

    pLS->layer     = resource->current.layer;
    pLS->popupHead = std::make_unique<CPopup>(pLS);
    pLS->monitorID = pMonitor->ID;
    pMonitor->m_aLayerSurfaceLayers[resource->current.layer].emplace_back(pLS);

    pLS->forceBlur = g_pConfigManager->shouldBlurLS(pLS->szNamespace);

    pLS->alpha.create(g_pConfigManager->getAnimationPropertyConfig("fadeLayersIn"), pLS, AVARDAMAGE_ENTIRE);
    pLS->realPosition.create(g_pConfigManager->getAnimationPropertyConfig("layersIn"), pLS, AVARDAMAGE_ENTIRE);
    pLS->realSize.create(g_pConfigManager->getAnimationPropertyConfig("layersIn"), pLS, AVARDAMAGE_ENTIRE);
    pLS->alpha.registerVar();
    pLS->realPosition.registerVar();
    pLS->realSize.registerVar();

    pLS->registerCallbacks();

    pLS->alpha.setValueAndWarp(0.f);

    Debug::log(LOG, "LayerSurface {:x} (namespace {} layer {}) created on monitor {}", (uintptr_t)resource.get(), resource->layerNamespace, (int)pLS->layer, pMonitor->szName);

    return pLS;
}

void CLayerSurface::registerCallbacks() {
    alpha.setUpdateCallback([this](void*) {
        if (dimAround)
            g_pHyprRenderer->damageMonitor(g_pCompositor->getMonitorFromID(monitorID));
    });
}

CLayerSurface::CLayerSurface(SP<CLayerShellResource> resource_) : layerSurface(resource_) {
    listeners.commit  = layerSurface->events.commit.registerListener([this](std::any d) { onCommit(); });
    listeners.map     = layerSurface->events.map.registerListener([this](std::any d) { onMap(); });
    listeners.unmap   = layerSurface->events.unmap.registerListener([this](std::any d) { onUnmap(); });
    listeners.destroy = layerSurface->events.destroy.registerListener([this](std::any d) { onDestroy(); });

    surface = CWLSurface::create();
}

CLayerSurface::~CLayerSurface() {
    if (!g_pHyprOpenGL)
        return;

    if (surface)
        surface->unassign();
    g_pHyprRenderer->makeEGLCurrent();
    std::erase_if(g_pHyprOpenGL->m_mLayerFramebuffers, [&](const auto& other) { return other.first.expired() || other.first.lock() == self.lock(); });
}

void CLayerSurface::onDestroy() {
    Debug::log(LOG, "LayerSurface {:x} destroyed", (uintptr_t)layerSurface.get());

    const auto PMONITOR = g_pCompositor->getMonitorFromID(monitorID);

    if (!g_pCompositor->getMonitorFromID(monitorID))
        Debug::log(WARN, "Layersurface destroyed on an invalid monitor (removed?)");

    if (!fadingOut) {
        if (mapped) {
            Debug::log(LOG, "Forcing an unmap of a LS that did a straight destroy!");
            onUnmap();
        } else {
            Debug::log(LOG, "Removing LayerSurface that wasn't mapped.");
            alpha.setValueAndWarp(0.f);
            fadingOut = true;
            g_pCompositor->addToFadingOutSafe(self.lock());
        }
    }

    popupHead.reset();

    noProcess = true;

    // rearrange to fix the reserved areas
    if (PMONITOR) {
        g_pHyprRenderer->arrangeLayersForMonitor(PMONITOR->ID);
        PMONITOR->scheduledRecalc = true;

        // and damage
        CBox geomFixed = {geometry.x + PMONITOR->vecPosition.x, geometry.y + PMONITOR->vecPosition.y, geometry.width, geometry.height};
        g_pHyprRenderer->damageBox(&geomFixed);
    }

    readyToDelete = true;
    layerSurface.reset();
    if (surface)
        surface->unassign();
}

void CLayerSurface::onMap() {
    Debug::log(LOG, "LayerSurface {:x} mapped", (uintptr_t)layerSurface.get());

    mapped        = true;
    interactivity = layerSurface->current.interactivity;

    layerSurface->surface->map();

    // this layer might be re-mapped.
    fadingOut = false;
    g_pCompositor->removeFromFadingOutSafe(self.lock());

    // fix if it changed its mon
    const auto PMONITOR = g_pCompositor->getMonitorFromID(monitorID);

    if (!PMONITOR)
        return;

    applyRules();

    PMONITOR->scheduledRecalc = true;

    g_pHyprRenderer->arrangeLayersForMonitor(PMONITOR->ID);

    surface->resource()->enter(PMONITOR->self.lock());

    const bool ISEXCLUSIVE = layerSurface->current.interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;

    if (ISEXCLUSIVE)
        g_pInputManager->m_dExclusiveLSes.push_back(self);

    const bool GRABSFOCUS = ISEXCLUSIVE ||
        (layerSurface->current.interactivity != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
         // don't focus if constrained
         (g_pSeatManager->mouse.expired() || !g_pInputManager->isConstrained()));

    if (GRABSFOCUS) {
        // TODO: use the new superb really very cool grab
        g_pSeatManager->setGrab(nullptr);
        g_pInputManager->releaseAllMouseButtons();
        g_pCompositor->focusSurface(surface->resource());

        const auto LOCAL = g_pInputManager->getMouseCoordsInternal() - Vector2D(geometry.x + PMONITOR->vecPosition.x, geometry.y + PMONITOR->vecPosition.y);
        g_pSeatManager->setPointerFocus(surface->resource(), LOCAL);
        g_pInputManager->m_bEmptyFocusCursorSet = false;
    }

    position = Vector2D(geometry.x, geometry.y);

    CBox geomFixed = {geometry.x + PMONITOR->vecPosition.x, geometry.y + PMONITOR->vecPosition.y, geometry.width, geometry.height};
    g_pHyprRenderer->damageBox(&geomFixed);
    const auto WORKSPACE  = PMONITOR->activeWorkspace;
    const bool FULLSCREEN = WORKSPACE->m_bHasFullscreenWindow && WORKSPACE->m_efFullscreenMode == FSMODE_FULLSCREEN;

    startAnimation(!(layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP && FULLSCREEN && !GRABSFOCUS));
    readyToDelete = false;
    fadingOut     = false;

    g_pEventManager->postEvent(SHyprIPCEvent{"openlayer", szNamespace});
    EMIT_HOOK_EVENT("openLayer", self.lock());

    g_pCompositor->setPreferredScaleForSurface(surface->resource(), PMONITOR->scale);
    g_pCompositor->setPreferredTransformForSurface(surface->resource(), PMONITOR->transform);
}

void CLayerSurface::onUnmap() {
    Debug::log(LOG, "LayerSurface {:x} unmapped", (uintptr_t)layerSurface.get());

    g_pEventManager->postEvent(SHyprIPCEvent{"closelayer", layerSurface->layerNamespace});
    EMIT_HOOK_EVENT("closeLayer", self.lock());

    std::erase_if(g_pInputManager->m_dExclusiveLSes, [this](const auto& other) { return !other.lock() || other.lock() == self.lock(); });

    if (!g_pCompositor->getMonitorFromID(monitorID) || g_pCompositor->m_bUnsafeState) {
        Debug::log(WARN, "Layersurface unmapping on invalid monitor (removed?) ignoring.");

        g_pCompositor->addToFadingOutSafe(self.lock());

        mapped = false;
        layerSurface->surface->unmap();

        startAnimation(false);
        return;
    }

    // make a snapshot and start fade
    g_pHyprOpenGL->makeLayerSnapshot(self.lock());

    startAnimation(false);

    mapped = false;
    layerSurface->surface->unmap();

    g_pCompositor->addToFadingOutSafe(self.lock());

    const auto PMONITOR = g_pCompositor->getMonitorFromID(monitorID);

    const bool WASLASTFOCUS = g_pCompositor->m_pLastFocus == surface->resource();

    if (!PMONITOR)
        return;

    // refocus if needed
    //                                vvvvvvvvvvvvv if there is a last focus and the last focus is not keyboard focusable, fallback to window
    if (WASLASTFOCUS || (g_pCompositor->m_pLastFocus && !g_pCompositor->m_pLastFocus->hlSurface->keyboardFocusable()))
        g_pInputManager->refocusLastWindow(PMONITOR);
    else if (g_pCompositor->m_pLastFocus)
        g_pSeatManager->setKeyboardFocus(g_pCompositor->m_pLastFocus.lock());

    CBox geomFixed = {geometry.x + PMONITOR->vecPosition.x, geometry.y + PMONITOR->vecPosition.y, geometry.width, geometry.height};
    g_pHyprRenderer->damageBox(&geomFixed);

    geomFixed = {geometry.x + (int)PMONITOR->vecPosition.x, geometry.y + (int)PMONITOR->vecPosition.y, (int)layerSurface->surface->current.size.x,
                 (int)layerSurface->surface->current.size.y};
    g_pHyprRenderer->damageBox(&geomFixed);

    g_pInputManager->sendMotionEventsToFocused();

    g_pHyprRenderer->arrangeLayersForMonitor(PMONITOR->ID);
}

void CLayerSurface::onCommit() {
    if (!layerSurface)
        return;

    if (!mapped) {
        // we're re-mapping if this is the case
        if (layerSurface->surface && !layerSurface->surface->current.buffer) {
            fadingOut = false;
            geometry  = {};
            g_pHyprRenderer->arrangeLayersForMonitor(monitorID);
        }

        return;
    }

    const auto PMONITOR = g_pCompositor->getMonitorFromID(monitorID);

    if (!PMONITOR)
        return;

    if (layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND || layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
        g_pHyprOpenGL->markBlurDirtyForMonitor(PMONITOR); // so that blur is recalc'd

    CBox geomFixed = {geometry.x, geometry.y, geometry.width, geometry.height};
    g_pHyprRenderer->damageBox(&geomFixed);

    if (layerSurface->current.committed != 0) {
        if (layerSurface->current.committed & CLayerShellResource::eCommittedState::STATE_LAYER) {

            for (auto it = PMONITOR->m_aLayerSurfaceLayers[layer].begin(); it != PMONITOR->m_aLayerSurfaceLayers[layer].end(); it++) {
                if (*it == self) {
                    if (layerSurface->current.layer == layer)
                        break;
                    PMONITOR->m_aLayerSurfaceLayers[layerSurface->current.layer].emplace_back(*it);
                    PMONITOR->m_aLayerSurfaceLayers[layer].erase(it);
                    break;
                }
            }

            layer = layerSurface->current.layer;

            if (layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND || layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
                g_pHyprOpenGL->markBlurDirtyForMonitor(PMONITOR); // so that blur is recalc'd
        }

        g_pHyprRenderer->arrangeLayersForMonitor(PMONITOR->ID);

        PMONITOR->scheduledRecalc = true;
    } else {
        position = Vector2D(geometry.x, geometry.y);

        // update geom if it changed
        if (layerSurface->surface->current.scale == 1 && PMONITOR->scale != 1.f && layerSurface->surface->current.viewport.hasDestination) {
            // fractional scaling. Dirty hack.
            geometry = {geometry.pos(), layerSurface->surface->current.viewport.destination};
        } else {
            // this is because some apps like e.g. rofi-lbonn can't fucking use the protocol correctly.
            geometry = {geometry.pos(), layerSurface->surface->current.size};
        }
    }

    if (realPosition.goal() != geometry.pos()) {
        if (realPosition.isBeingAnimated())
            realPosition = geometry.pos();
        else
            realPosition.setValueAndWarp(geometry.pos());
    }
    if (realSize.goal() != geometry.size()) {
        if (realSize.isBeingAnimated())
            realSize = geometry.size();
        else
            realSize.setValueAndWarp(geometry.size());
    }

    if (mapped) {
        const bool WASLASTFOCUS = g_pCompositor->m_pLastFocus == surface->resource();
        const bool WASEXCLUSIVE = interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
        const bool ISEXCLUSIVE  = layerSurface->current.interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;

        if (!WASEXCLUSIVE && ISEXCLUSIVE)
            g_pInputManager->m_dExclusiveLSes.push_back(self);
        else if (WASEXCLUSIVE && !ISEXCLUSIVE)
            std::erase_if(g_pInputManager->m_dExclusiveLSes, [this](const auto& other) { return !other.lock() || other.lock() == self.lock(); });

        // if the surface was focused and interactive but now isn't, refocus
        if (WASLASTFOCUS && !layerSurface->current.interactivity) {
            // moveMouseUnified won't focus non interactive layers but it won't unfocus them either,
            // so unfocus the surface here.
            g_pCompositor->focusSurface(nullptr);
            g_pInputManager->refocusLastWindow(g_pCompositor->getMonitorFromID(monitorID));
        } else if (!WASEXCLUSIVE && ISEXCLUSIVE) {
            // if now exclusive and not previously
            g_pSeatManager->setGrab(nullptr);
            g_pInputManager->releaseAllMouseButtons();
            g_pCompositor->focusSurface(surface->resource());

            const auto LOCAL = g_pInputManager->getMouseCoordsInternal() - Vector2D(geometry.x + PMONITOR->vecPosition.x, geometry.y + PMONITOR->vecPosition.y);
            g_pSeatManager->setPointerFocus(surface->resource(), LOCAL);
            g_pInputManager->m_bEmptyFocusCursorSet = false;
        }
    }

    interactivity = layerSurface->current.interactivity;

    g_pHyprRenderer->damageSurface(surface->resource(), position.x, position.y);

    g_pCompositor->setPreferredScaleForSurface(surface->resource(), PMONITOR->scale);
    g_pCompositor->setPreferredTransformForSurface(surface->resource(), PMONITOR->transform);
}

void CLayerSurface::applyRules() {
    noAnimations     = false;
    forceBlur        = false;
    ignoreAlpha      = false;
    ignoreAlphaValue = 0.f;
    dimAround        = false;
    xray             = -1;
    animationStyle.reset();

    for (auto& rule : g_pConfigManager->getMatchingRules(self.lock())) {
        if (rule.rule == "noanim")
            noAnimations = true;
        else if (rule.rule == "blur")
            forceBlur = true;
        else if (rule.rule == "blurpopups")
            forceBlurPopups = true;
        else if (rule.rule.starts_with("ignorealpha") || rule.rule.starts_with("ignorezero")) {
            const auto  FIRST_SPACE_POS = rule.rule.find_first_of(' ');
            std::string alphaValue      = "";
            if (FIRST_SPACE_POS != std::string::npos)
                alphaValue = rule.rule.substr(FIRST_SPACE_POS + 1);

            try {
                ignoreAlpha = true;
                if (!alphaValue.empty())
                    ignoreAlphaValue = std::stof(alphaValue);
            } catch (...) { Debug::log(ERR, "Invalid value passed to ignoreAlpha"); }
        } else if (rule.rule == "dimaround") {
            dimAround = true;
        } else if (rule.rule.starts_with("xray")) {
            CVarList vars{rule.rule, 0, ' '};
            try {
                xray = configStringToInt(vars[1]);
            } catch (...) {}
        } else if (rule.rule.starts_with("animation")) {
            CVarList vars{rule.rule, 2, 's'};
            animationStyle = vars[1];
        }
    }
}

void CLayerSurface::startAnimation(bool in, bool instant) {
    const auto ANIMSTYLE = animationStyle.value_or(realPosition.m_pConfig->pValues->internalStyle);
    if (in) {
        realPosition.m_pConfig = g_pConfigManager->getAnimationPropertyConfig("layersIn");
        realSize.m_pConfig     = g_pConfigManager->getAnimationPropertyConfig("layersIn");
        alpha.m_pConfig        = g_pConfigManager->getAnimationPropertyConfig("fadeLayersIn");
    } else {
        realPosition.m_pConfig = g_pConfigManager->getAnimationPropertyConfig("layersOut");
        realSize.m_pConfig     = g_pConfigManager->getAnimationPropertyConfig("layersOut");
        alpha.m_pConfig        = g_pConfigManager->getAnimationPropertyConfig("fadeLayersOut");
    }

    if (ANIMSTYLE.starts_with("slide")) {
        // get closest edge
        const auto MIDDLE = geometry.middle();

        const auto PMONITOR = g_pCompositor->getMonitorFromVector(MIDDLE);

        int        force = -1;

        CVarList   args(ANIMSTYLE, 0, 's');
        if (args.size() > 1) {
            const auto ARG2 = args[1];
            if (ARG2 == "top")
                force = 0;
            else if (ARG2 == "bottom")
                force = 1;
            else if (ARG2 == "left")
                force = 2;
            else if (ARG2 == "right")
                force = 3;
        }

        const std::array<Vector2D, 4> edgePoints = {
            PMONITOR->vecPosition + Vector2D{PMONITOR->vecSize.x / 2, 0.0},
            PMONITOR->vecPosition + Vector2D{PMONITOR->vecSize.x / 2, PMONITOR->vecSize.y},
            PMONITOR->vecPosition + Vector2D{0.0, PMONITOR->vecSize.y},
            PMONITOR->vecPosition + Vector2D{PMONITOR->vecSize.x, PMONITOR->vecSize.y / 2},
        };

        float closest = std::numeric_limits<float>::max();
        int   leader  = force;
        if (leader == -1) {
            for (size_t i = 0; i < 4; ++i) {
                float dist = MIDDLE.distance(edgePoints[i]);
                if (dist < closest) {
                    leader  = i;
                    closest = dist;
                }
            }
        }

        realSize.setValueAndWarp(geometry.size());
        alpha.setValueAndWarp(in ? 0.f : 1.f);
        alpha = in ? 1.f : 0.f;

        Vector2D prePos;

        switch (leader) {
            case 0:
                // TOP
                prePos = {geometry.x, PMONITOR->vecPosition.y - geometry.h};
                break;
            case 1:
                // BOTTOM
                prePos = {geometry.x, PMONITOR->vecPosition.y + PMONITOR->vecSize.y};
                break;
            case 2:
                // LEFT
                prePos = {PMONITOR->vecPosition.x - geometry.w, geometry.y};
                break;
            case 3:
                // RIGHT
                prePos = {PMONITOR->vecPosition.x + PMONITOR->vecSize.x, geometry.y};
                break;
            default: UNREACHABLE();
        }

        if (in) {
            realPosition.setValueAndWarp(prePos);
            realPosition = geometry.pos();
        } else {
            realPosition.setValueAndWarp(geometry.pos());
            realPosition = prePos;
        }

    } else if (ANIMSTYLE.starts_with("popin")) {
        float minPerc = 0.f;
        if (ANIMSTYLE.find("%") != std::string::npos) {
            try {
                auto percstr = ANIMSTYLE.substr(ANIMSTYLE.find_last_of(' '));
                minPerc      = std::stoi(percstr.substr(0, percstr.length() - 1));
            } catch (std::exception& e) {
                ; // oops
            }
        }

        minPerc *= 0.01;

        const auto GOALSIZE = (geometry.size() * minPerc).clamp({5, 5});
        const auto GOALPOS  = geometry.pos() + (geometry.size() - GOALSIZE) / 2.f;

        alpha.setValueAndWarp(in ? 0.f : 1.f);
        alpha = in ? 1.f : 0.f;

        if (in) {
            realSize.setValueAndWarp(GOALSIZE);
            realPosition.setValueAndWarp(GOALPOS);
            realSize     = geometry.size();
            realPosition = geometry.pos();
        } else {
            realSize.setValueAndWarp(geometry.size());
            realPosition.setValueAndWarp(geometry.pos());
            realSize     = GOALSIZE;
            realPosition = GOALPOS;
        }
    } else {
        // fade
        realPosition.setValueAndWarp(geometry.pos());
        realSize.setValueAndWarp(geometry.size());
        alpha = in ? 1.f : 0.f;
    }

    if (!in)
        fadingOut = true;
}

bool CLayerSurface::isFadedOut() {
    if (!fadingOut)
        return false;

    return !realPosition.isBeingAnimated() && !realSize.isBeingAnimated() && !alpha.isBeingAnimated();
}

int CLayerSurface::popupsCount() {
    if (!layerSurface || !mapped || fadingOut)
        return 0;

    int no = -1; // we have one dummy
    popupHead->breadthfirst([](CPopup* p, void* data) { *(int*)data += 1; }, &no);
    return no;
}
