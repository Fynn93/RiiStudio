#pragma once

#include "Model.hpp"
#include "Texture.hpp"

#include <LibCore/api/Node.hpp>
#include <LibCore/api/Collection.hpp>


namespace libcube { namespace jsystem {

//! Represents the state of a J3D model and textures (BMD, BDL) as well as animations.
//! Unlike the binary equivalents, per-material texture settings have been moved to samplers in materials.
//!
class J3DCollection final : public px::CollectionHost
{
	friend class J3DCollectionSpawner;
public:
	PX_TYPE_INFO("J3D Scene", "j3d_scene", "J::J3DCollection");

	
	J3DCollection()
		: px::CollectionHost({
			PX_GET_TID(J3DModel),
			PX_GET_TID(Texture)
		})
	{}

	PX_COLLECTION_FOLDER_GETTER(getModels, J3DModel);
	PX_COLLECTION_FOLDER_GETTER(getTextures, Texture);

	bool bdl = false;
};

class J3DCollectionSpawner : public px::IFactory
{
public:
	~J3DCollectionSpawner() override = default;
	J3DCollectionSpawner()
	{
		id = J3DCollection::TypeInfo.namespacedId;
	}

	px::Dynamic spawn() override
	{
		return px::make_dynamic<J3DCollection>();
	}
	std::unique_ptr<px::IFactory> clone() override
	{
		return std::make_unique<J3DCollectionSpawner>(*this);
	}
};

} } // namespace libcube::jsystem