// Generated with hyprwayland-scanner 0.4.5. Made with vaxry's keyboard and ❤️.
// wlr_data_control_unstable_v1

/*
 This protocol's authors' copyright notice is:


    Copyright © 2018 Simon Ser
    Copyright © 2019 Ivan Molodetskikh

    Permission to use, copy, modify, distribute, and sell this
    software and its documentation for any purpose is hereby granted
    without fee, provided that the above copyright notice appear in
    all copies and that both that copyright notice and this permission
    notice appear in supporting documentation, and that the name of
    the copyright holders not be used in advertising or publicity
    pertaining to distribution of the software without specific,
    written prior permission.  The copyright holders make no
    representations about the suitability of this software for any
    purpose.  It is provided "as is" without express or implied
    warranty.

    THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
    SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
    FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
    SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
    AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
    ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
    THIS SOFTWARE.

*/

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <wayland-client.h>

#define F std::function

struct wl_proxy;

class CCZwlrDataControlManagerV1;
class CCZwlrDataControlSourceV1;
class CCZwlrDataControlDeviceV1;
class CCWlSeat;
class CCZwlrDataControlDeviceV1;
class CCZwlrDataControlSourceV1;
class CCZwlrDataControlSourceV1;
class CCZwlrDataControlOfferV1;
class CCZwlrDataControlOfferV1;
class CCZwlrDataControlOfferV1;
class CCZwlrDataControlSourceV1;
class CCZwlrDataControlOfferV1;

#ifndef HYPRWAYLAND_SCANNER_NO_INTERFACES
extern const wl_interface zwlr_data_control_manager_v1_interface;
extern const wl_interface zwlr_data_control_device_v1_interface;
extern const wl_interface zwlr_data_control_source_v1_interface;
extern const wl_interface zwlr_data_control_offer_v1_interface;

#endif

class CCZwlrDataControlManagerV1 {
public:
    CCZwlrDataControlManagerV1(wl_proxy*);
    ~CCZwlrDataControlManagerV1();

    // set the data for this resource
    void setData(void* data) {
        {
            pData = data;
        }
    }

    // get the data for this resource
    void* data() {
        {
            return pData;
        }
    }

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {
        {
            return pResource;
        }
    }

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {
        {
            return pResource;
        }
    }

    // get the resource version
    int version() {
        {
            return wl_proxy_get_version(pResource);
        }
    }

    // --------------- Requests --------------- //

    // --------------- Events --------------- //

    wl_proxy* sendCreateDataSource();
    wl_proxy* sendGetDataDevice(wl_proxy*);
    void sendDestroy();

private:
    struct {
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};

class CCZwlrDataControlDeviceV1 {
public:
    CCZwlrDataControlDeviceV1(wl_proxy*);
    ~CCZwlrDataControlDeviceV1();

    // set the data for this resource
    void setData(void* data) {
        {
            pData = data;
        }
    }

    // get the data for this resource
    void* data() {
        {
            return pData;
        }
    }

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {
        {
            return pResource;
        }
    }

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {
        {
            return pResource;
        }
    }

    // get the resource version
    int version() {
        {
            return wl_proxy_get_version(pResource);
        }
    }

    // --------------- Requests --------------- //

    void setDataOffer(F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> handler);
    void setSelection(F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> handler);
    void setFinished(F<void(CCZwlrDataControlDeviceV1*)> handler);
    void setPrimarySelection(F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> handler);

    // --------------- Events --------------- //

    void sendSetSelection(CCZwlrDataControlSourceV1*);
    void sendDestroy();
    void sendSetPrimarySelection(CCZwlrDataControlSourceV1*);

private:
    struct {
        F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> dataOffer;
        F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> selection;
        F<void(CCZwlrDataControlDeviceV1*)> finished;
        F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> primarySelection;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};

class CCZwlrDataControlSourceV1 {
public:
    CCZwlrDataControlSourceV1(wl_proxy*);
    ~CCZwlrDataControlSourceV1();

    // set the data for this resource
    void setData(void* data) {
        {
            pData = data;
        }
    }

    // get the data for this resource
    void* data() {
        {
            return pData;
        }
    }

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {
        {
            return pResource;
        }
    }

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {
        {
            return pResource;
        }
    }

    // get the resource version
    int version() {
        {
            return wl_proxy_get_version(pResource);
        }
    }

    // --------------- Requests --------------- //

    void setSend(F<void(CCZwlrDataControlSourceV1*, const char*, int32_t)> handler);
    void setCancelled(F<void(CCZwlrDataControlSourceV1*)> handler);

    // --------------- Events --------------- //

    void sendOffer(const char*);
    void sendDestroy();

private:
    struct {
        F<void(CCZwlrDataControlSourceV1*, const char*, int32_t)> send;
        F<void(CCZwlrDataControlSourceV1*)> cancelled;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};

class CCZwlrDataControlOfferV1 {
public:
    CCZwlrDataControlOfferV1(wl_proxy*);
    ~CCZwlrDataControlOfferV1();

    // set the data for this resource
    void setData(void* data) {
        {
            pData = data;
        }
    }

    // get the data for this resource
    void* data() {
        {
            return pData;
        }
    }

    // get the raw wl_resource (wl_proxy) ptr
    wl_proxy* resource() {
        {
            return pResource;
        }
    }

    // get the raw wl_proxy ptr
    wl_proxy* proxy() {
        {
            return pResource;
        }
    }

    // get the resource version
    int version() {
        {
            return wl_proxy_get_version(pResource);
        }
    }

    // --------------- Requests --------------- //

    void setOffer(F<void(CCZwlrDataControlOfferV1*, const char*)> handler);

    // --------------- Events --------------- //

    void sendReceive(const char*, int32_t);
    void sendDestroy();

private:
    struct {
        F<void(CCZwlrDataControlOfferV1*, const char*)> offer;
    } requests;

    wl_proxy* pResource = nullptr;

    bool destroyed = false;

    void* pData = nullptr;
};

#undef F
