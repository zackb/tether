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

#define private public
#define HYPRWAYLAND_SCANNER_NO_INTERFACES
#include "wlr-data-control-unstable-v1.hpp"
#undef private
#define F std::function

static const wl_interface* wlrDataControlUnstableV1_dummyTypes[] = {nullptr};

// Reference all other interfaces.
// The reason why this is in snake is to
// be able to cooperate with existing
// wayland_scanner interfaces (they are interop)
extern const wl_interface zwlr_data_control_manager_v1_interface;
extern const wl_interface zwlr_data_control_device_v1_interface;
extern const wl_interface zwlr_data_control_source_v1_interface;
extern const wl_interface zwlr_data_control_offer_v1_interface;
extern const wl_interface wl_seat_interface;

static const void* _CCZwlrDataControlManagerV1VTable[] = {
    nullptr,
};

wl_proxy* CCZwlrDataControlManagerV1::sendCreateDataSource() {
    if (!pResource)
        return nullptr;

    auto proxy = wl_proxy_marshal_flags(
        pResource, 0, &zwlr_data_control_source_v1_interface, wl_proxy_get_version(pResource), 0, nullptr);

    return proxy;
}

wl_proxy* CCZwlrDataControlManagerV1::sendGetDataDevice(wl_proxy* seat) {
    if (!pResource)
        return nullptr;

    auto proxy = wl_proxy_marshal_flags(
        pResource, 1, &zwlr_data_control_device_v1_interface, wl_proxy_get_version(pResource), 0, nullptr, seat);

    return proxy;
}

void CCZwlrDataControlManagerV1::sendDestroy() {
    if (!pResource)
        return;
    destroyed = true;

    auto proxy = wl_proxy_marshal_flags(pResource, 2, nullptr, wl_proxy_get_version(pResource), 1);
    proxy;
}
static const wl_interface* _CZwlrDataControlManagerV1CreateDataSourceTypes[] = {
    &zwlr_data_control_source_v1_interface,
};
static const wl_interface* _CZwlrDataControlManagerV1GetDataDeviceTypes[] = {
    &zwlr_data_control_device_v1_interface,
    &wl_seat_interface,
};

static const wl_message _CZwlrDataControlManagerV1Requests[] = {
    {.name = "create_data_source", .signature = "n", .types = _CZwlrDataControlManagerV1CreateDataSourceTypes + 0},
    {.name = "get_data_device", .signature = "no", .types = _CZwlrDataControlManagerV1GetDataDeviceTypes + 0},
    {.name = "destroy", .signature = "", .types = wlrDataControlUnstableV1_dummyTypes + 0},
};

const wl_interface zwlr_data_control_manager_v1_interface = {
    .name = "zwlr_data_control_manager_v1",
    .version = 2,
    .method_count = 3,
    .methods = _CZwlrDataControlManagerV1Requests,
    .event_count = 0,
    .events = nullptr,
};

CCZwlrDataControlManagerV1::CCZwlrDataControlManagerV1(wl_proxy* resource) : pResource(resource) {

    if (!pResource)
        return;

    wl_proxy_add_listener(pResource, (void (**)(void))&_CCZwlrDataControlManagerV1VTable, this);
}

CCZwlrDataControlManagerV1::~CCZwlrDataControlManagerV1() {
    if (!destroyed)
        sendDestroy();
}

static void _CZwlrDataControlDeviceV1DataOffer(void* data, void* resource, wl_proxy* id) {
    const auto PO = (CCZwlrDataControlDeviceV1*)data;
    if (PO && PO->requests.dataOffer)
        PO->requests.dataOffer(PO, id);
}

static void _CZwlrDataControlDeviceV1Selection(void* data, void* resource, wl_proxy* id) {
    const auto PO = (CCZwlrDataControlDeviceV1*)data;
    if (PO && PO->requests.selection)
        PO->requests.selection(PO, id);
}

static void _CZwlrDataControlDeviceV1Finished(void* data, void* resource) {
    const auto PO = (CCZwlrDataControlDeviceV1*)data;
    if (PO && PO->requests.finished)
        PO->requests.finished(PO);
}

static void _CZwlrDataControlDeviceV1PrimarySelection(void* data, void* resource, wl_proxy* id) {
    const auto PO = (CCZwlrDataControlDeviceV1*)data;
    if (PO && PO->requests.primarySelection)
        PO->requests.primarySelection(PO, id);
}

static const void* _CCZwlrDataControlDeviceV1VTable[] = {
    (void*)_CZwlrDataControlDeviceV1DataOffer,
    (void*)_CZwlrDataControlDeviceV1Selection,
    (void*)_CZwlrDataControlDeviceV1Finished,
    (void*)_CZwlrDataControlDeviceV1PrimarySelection,
};

void CCZwlrDataControlDeviceV1::sendSetSelection(CCZwlrDataControlSourceV1* source) {
    if (!pResource)
        return;

    auto proxy = wl_proxy_marshal_flags(
        pResource, 0, nullptr, wl_proxy_get_version(pResource), 0, source ? source->pResource : nullptr);
    proxy;
}

void CCZwlrDataControlDeviceV1::sendDestroy() {
    if (!pResource)
        return;
    destroyed = true;

    auto proxy = wl_proxy_marshal_flags(pResource, 1, nullptr, wl_proxy_get_version(pResource), 1);
    proxy;
}

void CCZwlrDataControlDeviceV1::sendSetPrimarySelection(CCZwlrDataControlSourceV1* source) {
    if (!pResource)
        return;

    auto proxy = wl_proxy_marshal_flags(
        pResource, 2, nullptr, wl_proxy_get_version(pResource), 0, source ? source->pResource : nullptr);
    proxy;
}
static const wl_interface* _CZwlrDataControlDeviceV1SetSelectionTypes[] = {
    &zwlr_data_control_source_v1_interface,
};
static const wl_interface* _CZwlrDataControlDeviceV1SetPrimarySelectionTypes[] = {
    &zwlr_data_control_source_v1_interface,
};
static const wl_interface* _CZwlrDataControlDeviceV1DataOfferTypes[] = {
    &zwlr_data_control_offer_v1_interface,
};
static const wl_interface* _CZwlrDataControlDeviceV1SelectionTypes[] = {
    &zwlr_data_control_offer_v1_interface,
};
static const wl_interface* _CZwlrDataControlDeviceV1PrimarySelectionTypes[] = {
    &zwlr_data_control_offer_v1_interface,
};

static const wl_message _CZwlrDataControlDeviceV1Requests[] = {
    {.name = "set_selection", .signature = "?o", .types = _CZwlrDataControlDeviceV1SetSelectionTypes + 0},
    {.name = "destroy", .signature = "", .types = wlrDataControlUnstableV1_dummyTypes + 0},
    {.name = "set_primary_selection",
     .signature = "2?o",
     .types = _CZwlrDataControlDeviceV1SetPrimarySelectionTypes + 0},
};

static const wl_message _CZwlrDataControlDeviceV1Events[] = {
    {.name = "data_offer", .signature = "n", .types = _CZwlrDataControlDeviceV1DataOfferTypes + 0},
    {.name = "selection", .signature = "?o", .types = _CZwlrDataControlDeviceV1SelectionTypes + 0},
    {.name = "finished", .signature = "", .types = wlrDataControlUnstableV1_dummyTypes + 0},
    {.name = "primary_selection", .signature = "2?o", .types = _CZwlrDataControlDeviceV1PrimarySelectionTypes + 0},
};

const wl_interface zwlr_data_control_device_v1_interface = {
    .name = "zwlr_data_control_device_v1",
    .version = 2,
    .method_count = 3,
    .methods = _CZwlrDataControlDeviceV1Requests,
    .event_count = 4,
    .events = _CZwlrDataControlDeviceV1Events,
};

CCZwlrDataControlDeviceV1::CCZwlrDataControlDeviceV1(wl_proxy* resource) : pResource(resource) {

    if (!pResource)
        return;

    wl_proxy_add_listener(pResource, (void (**)(void))&_CCZwlrDataControlDeviceV1VTable, this);
}

CCZwlrDataControlDeviceV1::~CCZwlrDataControlDeviceV1() {
    if (!destroyed)
        sendDestroy();
}

void CCZwlrDataControlDeviceV1::setDataOffer(F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> handler) {
    requests.dataOffer = handler;
}

void CCZwlrDataControlDeviceV1::setSelection(F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> handler) {
    requests.selection = handler;
}

void CCZwlrDataControlDeviceV1::setFinished(F<void(CCZwlrDataControlDeviceV1*)> handler) {
    requests.finished = handler;
}

void CCZwlrDataControlDeviceV1::setPrimarySelection(F<void(CCZwlrDataControlDeviceV1*, wl_proxy*)> handler) {
    requests.primarySelection = handler;
}

static void _CZwlrDataControlSourceV1Send(void* data, void* resource, const char* mime_type, int32_t fd) {
    const auto PO = (CCZwlrDataControlSourceV1*)data;
    if (PO && PO->requests.send)
        PO->requests.send(PO, mime_type, fd);
}

static void _CZwlrDataControlSourceV1Cancelled(void* data, void* resource) {
    const auto PO = (CCZwlrDataControlSourceV1*)data;
    if (PO && PO->requests.cancelled)
        PO->requests.cancelled(PO);
}

static const void* _CCZwlrDataControlSourceV1VTable[] = {
    (void*)_CZwlrDataControlSourceV1Send,
    (void*)_CZwlrDataControlSourceV1Cancelled,
};

void CCZwlrDataControlSourceV1::sendOffer(const char* mime_type) {
    if (!pResource)
        return;

    auto proxy = wl_proxy_marshal_flags(pResource, 0, nullptr, wl_proxy_get_version(pResource), 0, mime_type);
    proxy;
}

void CCZwlrDataControlSourceV1::sendDestroy() {
    if (!pResource)
        return;
    destroyed = true;

    auto proxy = wl_proxy_marshal_flags(pResource, 1, nullptr, wl_proxy_get_version(pResource), 1);
    proxy;
}
static const wl_interface* _CZwlrDataControlSourceV1OfferTypes[] = {
    nullptr,
};
static const wl_interface* _CZwlrDataControlSourceV1SendTypes[] = {
    nullptr,
    nullptr,
};

static const wl_message _CZwlrDataControlSourceV1Requests[] = {
    {.name = "offer", .signature = "s", .types = _CZwlrDataControlSourceV1OfferTypes + 0},
    {.name = "destroy", .signature = "", .types = wlrDataControlUnstableV1_dummyTypes + 0},
};

static const wl_message _CZwlrDataControlSourceV1Events[] = {
    {.name = "send", .signature = "sh", .types = _CZwlrDataControlSourceV1SendTypes + 0},
    {.name = "cancelled", .signature = "", .types = wlrDataControlUnstableV1_dummyTypes + 0},
};

const wl_interface zwlr_data_control_source_v1_interface = {
    .name = "zwlr_data_control_source_v1",
    .version = 1,
    .method_count = 2,
    .methods = _CZwlrDataControlSourceV1Requests,
    .event_count = 2,
    .events = _CZwlrDataControlSourceV1Events,
};

CCZwlrDataControlSourceV1::CCZwlrDataControlSourceV1(wl_proxy* resource) : pResource(resource) {

    if (!pResource)
        return;

    wl_proxy_add_listener(pResource, (void (**)(void))&_CCZwlrDataControlSourceV1VTable, this);
}

CCZwlrDataControlSourceV1::~CCZwlrDataControlSourceV1() {
    if (!destroyed)
        sendDestroy();
}

void CCZwlrDataControlSourceV1::setSend(F<void(CCZwlrDataControlSourceV1*, const char*, int32_t)> handler) {
    requests.send = handler;
}

void CCZwlrDataControlSourceV1::setCancelled(F<void(CCZwlrDataControlSourceV1*)> handler) {
    requests.cancelled = handler;
}

static void _CZwlrDataControlOfferV1Offer(void* data, void* resource, const char* mime_type) {
    const auto PO = (CCZwlrDataControlOfferV1*)data;
    if (PO && PO->requests.offer)
        PO->requests.offer(PO, mime_type);
}

static const void* _CCZwlrDataControlOfferV1VTable[] = {
    (void*)_CZwlrDataControlOfferV1Offer,
};

void CCZwlrDataControlOfferV1::sendReceive(const char* mime_type, int32_t fd) {
    if (!pResource)
        return;

    auto proxy = wl_proxy_marshal_flags(pResource, 0, nullptr, wl_proxy_get_version(pResource), 0, mime_type, fd);
    proxy;
}

void CCZwlrDataControlOfferV1::sendDestroy() {
    if (!pResource)
        return;
    destroyed = true;

    auto proxy = wl_proxy_marshal_flags(pResource, 1, nullptr, wl_proxy_get_version(pResource), 1);
    proxy;
}
static const wl_interface* _CZwlrDataControlOfferV1ReceiveTypes[] = {
    nullptr,
    nullptr,
};
static const wl_interface* _CZwlrDataControlOfferV1OfferTypes[] = {
    nullptr,
};

static const wl_message _CZwlrDataControlOfferV1Requests[] = {
    {.name = "receive", .signature = "sh", .types = _CZwlrDataControlOfferV1ReceiveTypes + 0},
    {.name = "destroy", .signature = "", .types = wlrDataControlUnstableV1_dummyTypes + 0},
};

static const wl_message _CZwlrDataControlOfferV1Events[] = {
    {.name = "offer", .signature = "s", .types = _CZwlrDataControlOfferV1OfferTypes + 0},
};

const wl_interface zwlr_data_control_offer_v1_interface = {
    .name = "zwlr_data_control_offer_v1",
    .version = 1,
    .method_count = 2,
    .methods = _CZwlrDataControlOfferV1Requests,
    .event_count = 1,
    .events = _CZwlrDataControlOfferV1Events,
};

CCZwlrDataControlOfferV1::CCZwlrDataControlOfferV1(wl_proxy* resource) : pResource(resource) {

    if (!pResource)
        return;

    wl_proxy_add_listener(pResource, (void (**)(void))&_CCZwlrDataControlOfferV1VTable, this);
}

CCZwlrDataControlOfferV1::~CCZwlrDataControlOfferV1() {
    if (!destroyed)
        sendDestroy();
}

void CCZwlrDataControlOfferV1::setOffer(F<void(CCZwlrDataControlOfferV1*, const char*)> handler) {
    requests.offer = handler;
}

#undef F
