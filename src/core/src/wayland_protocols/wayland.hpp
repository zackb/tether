// Generated with hyprwayland-scanner 0.4.5. Made with vaxry's keyboard and ❤️.
// wayland

/*
 This protocol's authors' copyright notice is:


    Copyright © 2008-2011 Kristian Høgsberg
    Copyright © 2010-2011 Intel Corporation
    Copyright © 2012-2013 Collabora, Ltd.

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation files
    (the "Software"), to deal in the Software without restriction,
    including without limitation the rights to use, copy, modify, merge,
    publish, distribute, sublicense, and/or sell copies of the Software,
    and to permit persons to whom the Software is furnished to do so,
    subject to the following conditions:

    The above copyright notice and this permission notice (including the
    next paragraph) shall be included in all copies or substantial
    portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
    BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
    ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
  
*/

#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <wayland-client.h>

#define F std::function

struct wl_proxy;


class CCWlDisplay;
class CCWlCallback;
class CCWlRegistry;
class CCWlRegistry;
class CCWlCallback;
class CCWlCompositor;
class CCWlSurface;
class CCWlRegion;
class CCWlShmPool;
class CCWlBuffer;
class CCWlShm;
class CCWlShmPool;
class CCWlBuffer;
class CCWlDataOffer;
class CCWlDataSource;
class CCWlDataDevice;
class CCWlDataSource;
class CCWlSurface;
class CCWlSurface;
class CCWlDataSource;
class CCWlDataOffer;
class CCWlSurface;
class CCWlDataOffer;
class CCWlDataOffer;
class CCWlDataDeviceManager;
class CCWlDataSource;
class CCWlDataDevice;
class CCWlSeat;
class CCWlShell;
class CCWlShellSurface;
class CCWlSurface;
class CCWlShellSurface;
class CCWlSeat;
class CCWlSeat;
class CCWlSurface;
class CCWlOutput;
class CCWlSeat;
class CCWlSurface;
class CCWlOutput;
class CCWlSurface;
class CCWlBuffer;
class CCWlCallback;
class CCWlRegion;
class CCWlRegion;
class CCWlOutput;
class CCWlOutput;
class CCWlSeat;
class CCWlPointer;
class CCWlKeyboard;
class CCWlTouch;
class CCWlPointer;
class CCWlSurface;
class CCWlSurface;
class CCWlSurface;
class CCWlKeyboard;
class CCWlSurface;
class CCWlSurface;
class CCWlTouch;
class CCWlSurface;
class CCWlOutput;
class CCWlRegion;
class CCWlSubcompositor;
class CCWlSubsurface;
class CCWlSurface;
class CCWlSurface;
class CCWlSubsurface;
class CCWlSurface;
class CCWlSurface;
class CCWlFixes;
class CCWlRegistry;

#ifndef HYPRWAYLAND_SCANNER_NO_INTERFACES
extern const wl_interface wl_display_interface;
extern const wl_interface wl_registry_interface;
extern const wl_interface wl_callback_interface;
extern const wl_interface wl_compositor_interface;
extern const wl_interface wl_shm_pool_interface;
extern const wl_interface wl_shm_interface;
extern const wl_interface wl_buffer_interface;
extern const wl_interface wl_data_offer_interface;
extern const wl_interface wl_data_source_interface;
extern const wl_interface wl_data_device_interface;
extern const wl_interface wl_data_device_manager_interface;
extern const wl_interface wl_shell_interface;
extern const wl_interface wl_shell_surface_interface;
extern const wl_interface wl_surface_interface;
extern const wl_interface wl_seat_interface;
extern const wl_interface wl_pointer_interface;
extern const wl_interface wl_keyboard_interface;
extern const wl_interface wl_touch_interface;
extern const wl_interface wl_output_interface;
extern const wl_interface wl_region_interface;
extern const wl_interface wl_subcompositor_interface;
extern const wl_interface wl_subsurface_interface;
extern const wl_interface wl_fixes_interface;

#endif


class CCWlDisplay {
  public:
    CCWlDisplay(wl_proxy*);
    ~CCWlDisplay();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setError(F<void(CCWlDisplay*, wl_proxy*, uint32_t, const char*)> handler);
    void setDeleteId(F<void(CCWlDisplay*, uint32_t)> handler);

    // --------------- Events --------------- //

    wl_proxy* sendSync();
    wl_proxy* sendGetRegistry();

  private:
    struct {
        F<void(CCWlDisplay*, wl_proxy*, uint32_t, const char*)> error;
        F<void(CCWlDisplay*, uint32_t)> deleteId;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlRegistry {
  public:
    CCWlRegistry(wl_proxy*);
    ~CCWlRegistry();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setGlobal(F<void(CCWlRegistry*, uint32_t, const char*, uint32_t)> handler);
    void setGlobalRemove(F<void(CCWlRegistry*, uint32_t)> handler);

    // --------------- Events --------------- //

    void sendBind(uint32_t);

  private:
    struct {
        F<void(CCWlRegistry*, uint32_t, const char*, uint32_t)> global;
        F<void(CCWlRegistry*, uint32_t)> globalRemove;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlCallback {
  public:
    CCWlCallback(wl_proxy*);
    ~CCWlCallback();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setDone(F<void(CCWlCallback*, uint32_t)> handler);

    // --------------- Events --------------- //


  private:
    struct {
        F<void(CCWlCallback*, uint32_t)> done;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlCompositor {
  public:
    CCWlCompositor(wl_proxy*);
    ~CCWlCompositor();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    wl_proxy* sendCreateSurface();
    wl_proxy* sendCreateRegion();

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlShmPool {
  public:
    CCWlShmPool(wl_proxy*);
    ~CCWlShmPool();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    wl_proxy* sendCreateBuffer(int32_t, int32_t, int32_t, int32_t, uint32_t);
    void sendDestroy();
    void sendResize(int32_t);

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlShm {
  public:
    CCWlShm(wl_proxy*);
    ~CCWlShm();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setFormat(F<void(CCWlShm*, enum wl_shm_format)> handler);

    // --------------- Events --------------- //

    wl_proxy* sendCreatePool(int32_t, int32_t);
    void sendRelease();

  private:
    struct {
        F<void(CCWlShm*, enum wl_shm_format)> format;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlBuffer {
  public:
    CCWlBuffer(wl_proxy*);
    ~CCWlBuffer();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setRelease(F<void(CCWlBuffer*)> handler);

    // --------------- Events --------------- //

    void sendDestroy();

  private:
    struct {
        F<void(CCWlBuffer*)> release;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlDataOffer {
  public:
    CCWlDataOffer(wl_proxy*);
    ~CCWlDataOffer();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setOffer(F<void(CCWlDataOffer*, const char*)> handler);
    void setSourceActions(F<void(CCWlDataOffer*, uint32_t)> handler);
    void setAction(F<void(CCWlDataOffer*, uint32_t)> handler);

    // --------------- Events --------------- //

    void sendAccept(uint32_t, const char*);
    void sendReceive(const char*, int32_t);
    void sendDestroy();
    void sendFinish();
    void sendSetActions(uint32_t, uint32_t);

  private:
    struct {
        F<void(CCWlDataOffer*, const char*)> offer;
        F<void(CCWlDataOffer*, uint32_t)> sourceActions;
        F<void(CCWlDataOffer*, uint32_t)> action;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlDataSource {
  public:
    CCWlDataSource(wl_proxy*);
    ~CCWlDataSource();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setTarget(F<void(CCWlDataSource*, const char*)> handler);
    void setSend(F<void(CCWlDataSource*, const char*, int32_t)> handler);
    void setCancelled(F<void(CCWlDataSource*)> handler);
    void setDndDropPerformed(F<void(CCWlDataSource*)> handler);
    void setDndFinished(F<void(CCWlDataSource*)> handler);
    void setAction(F<void(CCWlDataSource*, uint32_t)> handler);

    // --------------- Events --------------- //

    void sendOffer(const char*);
    void sendDestroy();
    void sendSetActions(uint32_t);

  private:
    struct {
        F<void(CCWlDataSource*, const char*)> target;
        F<void(CCWlDataSource*, const char*, int32_t)> send;
        F<void(CCWlDataSource*)> cancelled;
        F<void(CCWlDataSource*)> dndDropPerformed;
        F<void(CCWlDataSource*)> dndFinished;
        F<void(CCWlDataSource*, uint32_t)> action;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlDataDevice {
  public:
    CCWlDataDevice(wl_proxy*);
    ~CCWlDataDevice();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setDataOffer(F<void(CCWlDataDevice*, wl_proxy*)> handler);
    void setEnter(F<void(CCWlDataDevice*, uint32_t, wl_proxy*, wl_fixed_t, wl_fixed_t, wl_proxy*)> handler);
    void setLeave(F<void(CCWlDataDevice*)> handler);
    void setMotion(F<void(CCWlDataDevice*, uint32_t, wl_fixed_t, wl_fixed_t)> handler);
    void setDrop(F<void(CCWlDataDevice*)> handler);
    void setSelection(F<void(CCWlDataDevice*, wl_proxy*)> handler);

    // --------------- Events --------------- //

    void sendStartDrag(CCWlDataSource*, CCWlSurface*, CCWlSurface*, uint32_t);
    void sendSetSelection(CCWlDataSource*, uint32_t);
    void sendRelease();

  private:
    struct {
        F<void(CCWlDataDevice*, wl_proxy*)> dataOffer;
        F<void(CCWlDataDevice*, uint32_t, wl_proxy*, wl_fixed_t, wl_fixed_t, wl_proxy*)> enter;
        F<void(CCWlDataDevice*)> leave;
        F<void(CCWlDataDevice*, uint32_t, wl_fixed_t, wl_fixed_t)> motion;
        F<void(CCWlDataDevice*)> drop;
        F<void(CCWlDataDevice*, wl_proxy*)> selection;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlDataDeviceManager {
  public:
    CCWlDataDeviceManager(wl_proxy*);
    ~CCWlDataDeviceManager();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    wl_proxy* sendCreateDataSource();
    wl_proxy* sendGetDataDevice(CCWlSeat*);

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlShell {
  public:
    CCWlShell(wl_proxy*);
    ~CCWlShell();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    wl_proxy* sendGetShellSurface(CCWlSurface*);

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlShellSurface {
  public:
    CCWlShellSurface(wl_proxy*);
    ~CCWlShellSurface();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setPing(F<void(CCWlShellSurface*, uint32_t)> handler);
    void setConfigure(F<void(CCWlShellSurface*, enum wl_shell_surface_resize, int32_t, int32_t)> handler);
    void setPopupDone(F<void(CCWlShellSurface*)> handler);

    // --------------- Events --------------- //

    void sendPong(uint32_t);
    void sendMove(CCWlSeat*, uint32_t);
    void sendResize(CCWlSeat*, uint32_t, enum wl_shell_surface_resize);
    void sendSetToplevel();
    void sendSetTransient(CCWlSurface*, int32_t, int32_t, enum wl_shell_surface_transient);
    void sendSetFullscreen(enum wl_shell_surface_fullscreen_method, uint32_t, CCWlOutput*);
    void sendSetPopup(CCWlSeat*, uint32_t, CCWlSurface*, int32_t, int32_t, enum wl_shell_surface_transient);
    void sendSetMaximized(CCWlOutput*);
    void sendSetTitle(const char*);
    void sendSetClass(const char*);

  private:
    struct {
        F<void(CCWlShellSurface*, uint32_t)> ping;
        F<void(CCWlShellSurface*, enum wl_shell_surface_resize, int32_t, int32_t)> configure;
        F<void(CCWlShellSurface*)> popupDone;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlSurface {
  public:
    CCWlSurface(wl_proxy*);
    ~CCWlSurface();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setEnter(F<void(CCWlSurface*, wl_proxy*)> handler);
    void setLeave(F<void(CCWlSurface*, wl_proxy*)> handler);
    void setPreferredBufferScale(F<void(CCWlSurface*, int32_t)> handler);
    void setPreferredBufferTransform(F<void(CCWlSurface*, uint32_t)> handler);

    // --------------- Events --------------- //

    void sendDestroy();
    void sendAttach(CCWlBuffer*, int32_t, int32_t);
    void sendDamage(int32_t, int32_t, int32_t, int32_t);
    wl_proxy* sendFrame();
    void sendSetOpaqueRegion(CCWlRegion*);
    void sendSetInputRegion(CCWlRegion*);
    void sendCommit();
    void sendSetBufferTransform(int32_t);
    void sendSetBufferScale(int32_t);
    void sendDamageBuffer(int32_t, int32_t, int32_t, int32_t);
    void sendOffset(int32_t, int32_t);

  private:
    struct {
        F<void(CCWlSurface*, wl_proxy*)> enter;
        F<void(CCWlSurface*, wl_proxy*)> leave;
        F<void(CCWlSurface*, int32_t)> preferredBufferScale;
        F<void(CCWlSurface*, uint32_t)> preferredBufferTransform;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlSeat {
  public:
    CCWlSeat(wl_proxy*);
    ~CCWlSeat();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setCapabilities(F<void(CCWlSeat*, enum wl_seat_capability)> handler);
    void setName(F<void(CCWlSeat*, const char*)> handler);

    // --------------- Events --------------- //

    wl_proxy* sendGetPointer();
    wl_proxy* sendGetKeyboard();
    wl_proxy* sendGetTouch();
    void sendRelease();

  private:
    struct {
        F<void(CCWlSeat*, enum wl_seat_capability)> capabilities;
        F<void(CCWlSeat*, const char*)> name;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlPointer {
  public:
    CCWlPointer(wl_proxy*);
    ~CCWlPointer();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setEnter(F<void(CCWlPointer*, uint32_t, wl_proxy*, wl_fixed_t, wl_fixed_t)> handler);
    void setLeave(F<void(CCWlPointer*, uint32_t, wl_proxy*)> handler);
    void setMotion(F<void(CCWlPointer*, uint32_t, wl_fixed_t, wl_fixed_t)> handler);
    void setButton(F<void(CCWlPointer*, uint32_t, uint32_t, uint32_t, enum wl_pointer_button_state)> handler);
    void setAxis(F<void(CCWlPointer*, uint32_t, enum wl_pointer_axis, wl_fixed_t)> handler);
    void setFrame(F<void(CCWlPointer*)> handler);
    void setAxisSource(F<void(CCWlPointer*, enum wl_pointer_axis_source)> handler);
    void setAxisStop(F<void(CCWlPointer*, uint32_t, enum wl_pointer_axis)> handler);
    void setAxisDiscrete(F<void(CCWlPointer*, enum wl_pointer_axis, int32_t)> handler);
    void setAxisValue120(F<void(CCWlPointer*, enum wl_pointer_axis, int32_t)> handler);
    void setAxisRelativeDirection(F<void(CCWlPointer*, enum wl_pointer_axis, enum wl_pointer_axis_relative_direction)> handler);

    // --------------- Events --------------- //

    void sendSetCursor(uint32_t, CCWlSurface*, int32_t, int32_t);
    void sendRelease();

  private:
    struct {
        F<void(CCWlPointer*, uint32_t, wl_proxy*, wl_fixed_t, wl_fixed_t)> enter;
        F<void(CCWlPointer*, uint32_t, wl_proxy*)> leave;
        F<void(CCWlPointer*, uint32_t, wl_fixed_t, wl_fixed_t)> motion;
        F<void(CCWlPointer*, uint32_t, uint32_t, uint32_t, enum wl_pointer_button_state)> button;
        F<void(CCWlPointer*, uint32_t, enum wl_pointer_axis, wl_fixed_t)> axis;
        F<void(CCWlPointer*)> frame;
        F<void(CCWlPointer*, enum wl_pointer_axis_source)> axisSource;
        F<void(CCWlPointer*, uint32_t, enum wl_pointer_axis)> axisStop;
        F<void(CCWlPointer*, enum wl_pointer_axis, int32_t)> axisDiscrete;
        F<void(CCWlPointer*, enum wl_pointer_axis, int32_t)> axisValue120;
        F<void(CCWlPointer*, enum wl_pointer_axis, enum wl_pointer_axis_relative_direction)> axisRelativeDirection;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlKeyboard {
  public:
    CCWlKeyboard(wl_proxy*);
    ~CCWlKeyboard();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setKeymap(F<void(CCWlKeyboard*, enum wl_keyboard_keymap_format, int32_t, uint32_t)> handler);
    void setEnter(F<void(CCWlKeyboard*, uint32_t, wl_proxy*, wl_array*)> handler);
    void setLeave(F<void(CCWlKeyboard*, uint32_t, wl_proxy*)> handler);
    void setKey(F<void(CCWlKeyboard*, uint32_t, uint32_t, uint32_t, enum wl_keyboard_key_state)> handler);
    void setModifiers(F<void(CCWlKeyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)> handler);
    void setRepeatInfo(F<void(CCWlKeyboard*, int32_t, int32_t)> handler);

    // --------------- Events --------------- //

    void sendRelease();

  private:
    struct {
        F<void(CCWlKeyboard*, enum wl_keyboard_keymap_format, int32_t, uint32_t)> keymap;
        F<void(CCWlKeyboard*, uint32_t, wl_proxy*, wl_array*)> enter;
        F<void(CCWlKeyboard*, uint32_t, wl_proxy*)> leave;
        F<void(CCWlKeyboard*, uint32_t, uint32_t, uint32_t, enum wl_keyboard_key_state)> key;
        F<void(CCWlKeyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)> modifiers;
        F<void(CCWlKeyboard*, int32_t, int32_t)> repeatInfo;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlTouch {
  public:
    CCWlTouch(wl_proxy*);
    ~CCWlTouch();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setDown(F<void(CCWlTouch*, uint32_t, uint32_t, wl_proxy*, int32_t, wl_fixed_t, wl_fixed_t)> handler);
    void setUp(F<void(CCWlTouch*, uint32_t, uint32_t, int32_t)> handler);
    void setMotion(F<void(CCWlTouch*, uint32_t, int32_t, wl_fixed_t, wl_fixed_t)> handler);
    void setFrame(F<void(CCWlTouch*)> handler);
    void setCancel(F<void(CCWlTouch*)> handler);
    void setShape(F<void(CCWlTouch*, int32_t, wl_fixed_t, wl_fixed_t)> handler);
    void setOrientation(F<void(CCWlTouch*, int32_t, wl_fixed_t)> handler);

    // --------------- Events --------------- //

    void sendRelease();

  private:
    struct {
        F<void(CCWlTouch*, uint32_t, uint32_t, wl_proxy*, int32_t, wl_fixed_t, wl_fixed_t)> down;
        F<void(CCWlTouch*, uint32_t, uint32_t, int32_t)> up;
        F<void(CCWlTouch*, uint32_t, int32_t, wl_fixed_t, wl_fixed_t)> motion;
        F<void(CCWlTouch*)> frame;
        F<void(CCWlTouch*)> cancel;
        F<void(CCWlTouch*, int32_t, wl_fixed_t, wl_fixed_t)> shape;
        F<void(CCWlTouch*, int32_t, wl_fixed_t)> orientation;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlOutput {
  public:
    CCWlOutput(wl_proxy*);
    ~CCWlOutput();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //

    void setGeometry(F<void(CCWlOutput*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t)> handler);
    void setMode(F<void(CCWlOutput*, enum wl_output_mode, int32_t, int32_t, int32_t)> handler);
    void setDone(F<void(CCWlOutput*)> handler);
    void setScale(F<void(CCWlOutput*, int32_t)> handler);
    void setName(F<void(CCWlOutput*, const char*)> handler);
    void setDescription(F<void(CCWlOutput*, const char*)> handler);

    // --------------- Events --------------- //

    void sendRelease();

  private:
    struct {
        F<void(CCWlOutput*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t)> geometry;
        F<void(CCWlOutput*, enum wl_output_mode, int32_t, int32_t, int32_t)> mode;
        F<void(CCWlOutput*)> done;
        F<void(CCWlOutput*, int32_t)> scale;
        F<void(CCWlOutput*, const char*)> name;
        F<void(CCWlOutput*, const char*)> description;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlRegion {
  public:
    CCWlRegion(wl_proxy*);
    ~CCWlRegion();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    void sendDestroy();
    void sendAdd(int32_t, int32_t, int32_t, int32_t);
    void sendSubtract(int32_t, int32_t, int32_t, int32_t);

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlSubcompositor {
  public:
    CCWlSubcompositor(wl_proxy*);
    ~CCWlSubcompositor();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    void sendDestroy();
    wl_proxy* sendGetSubsurface(CCWlSurface*, CCWlSurface*);

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlSubsurface {
  public:
    CCWlSubsurface(wl_proxy*);
    ~CCWlSubsurface();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    void sendDestroy();
    void sendSetPosition(int32_t, int32_t);
    void sendPlaceAbove(CCWlSurface*);
    void sendPlaceBelow(CCWlSurface*);
    void sendSetSync();
    void sendSetDesync();

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



class CCWlFixes {
  public:
    CCWlFixes(wl_proxy*);
    ~CCWlFixes();


    // set the data for this resource
    void setData(void* data) {{
        pData = data;
    }}

    // get the data for this resource
    void* data() {{
        return pData;
    }}

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {{
        return pResource;
    }}

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {{
        return pResource;
    }}

    // get the resource version
    int version() {{
        return wl_proxy_get_version(pResource);
    }}
            
    // --------------- Requests --------------- //


    // --------------- Events --------------- //

    void sendDestroy();
    void sendDestroyRegistry(CCWlRegistry*);

  private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};



#undef F
