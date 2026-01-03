#pragma once

struct CoreState {
    XrSession Session = XR_NULL_HANDLE;

    // Handtracking
    PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_ = nullptr;
    PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT_ = nullptr;
    PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_ = nullptr;

    // Passthrough
    XrPassthroughFB Passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB PassthroughLayer = XR_NULL_HANDLE;

    // XR_FB_passthrough
    PFN_xrCreatePassthroughFB XrCreatePassthroughFB = nullptr;
    PFN_xrDestroyPassthroughFB XrDestroyPassthroughFB = nullptr;
    PFN_xrPassthroughStartFB XrPassthroughStartFB = nullptr;
    PFN_xrPassthroughPauseFB XrPassthroughPauseFB = nullptr;
    PFN_xrCreatePassthroughLayerFB XrCreatePassthroughLayerFB = nullptr;
    PFN_xrDestroyPassthroughLayerFB XrDestroyPassthroughLayerFB = nullptr;
    PFN_xrPassthroughLayerSetStyleFB XrPassthroughLayerSetStyleFB = nullptr;
    PFN_xrCreateGeometryInstanceFB XrCreateGeometryInstanceFB = nullptr;
    PFN_xrDestroyGeometryInstanceFB XrDestroyGeometryInstanceFB = nullptr;
    PFN_xrGeometryInstanceSetTransformFB XrGeometryInstanceSetTransformFB = nullptr;

    // XR_FB_triangle_mesh
    PFN_xrCreateTriangleMeshFB XrCreateTriangleMeshFB = nullptr;
    PFN_xrDestroyTriangleMeshFB XrDestroyTriangleMeshFB = nullptr;
};