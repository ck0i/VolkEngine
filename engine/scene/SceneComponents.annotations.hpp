#pragma once

#define VE_SCENE_COMPONENT(...)
#define VE_SCENE_PROPERTY(...)

// Parsed by tools/generate_scene_schema.py. This file is not included directly.
VE_SCENE_COMPONENT(Transform, ve.scene.transform, 2)
VE_SCENE_PROPERTY(Transform, Translation, translation, Vec3, -100000, 100000, 0.1, System.Numerics.Vector3)
VE_SCENE_PROPERTY(Transform, Rotation, rotation, Quaternion, -1, 1, 0.01, System.Numerics.Quaternion)
VE_SCENE_PROPERTY(Transform, Scale, scale, Vec3, -10000, 10000, 0.01, System.Numerics.Vector3)
VE_SCENE_COMPONENT(Renderable, ve.scene.renderable, 1)
VE_SCENE_PROPERTY(Renderable, Mesh, mesh, AssetId, 0, 0, 0, VolkEngine.AssetId)
VE_SCENE_PROPERTY(Renderable, Material, material, AssetId, 0, 0, 0, VolkEngine.AssetId)
VE_SCENE_PROPERTY(Renderable, Visible, visible, Bool, 0, 1, 1, System.Boolean)

#undef VE_SCENE_PROPERTY
#undef VE_SCENE_COMPONENT
