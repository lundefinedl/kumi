#pragma once

#include "property.hpp"

struct MaterialProperty {

	MaterialProperty(const string &name, int value) : name(name), type(PropertyType::kInt), _int(value) {}
	MaterialProperty(const string &name, float value) : name(name), type(PropertyType::kFloat) { _float[0] = value; }
	MaterialProperty(const string &name, const XMFLOAT4 &value) : name(name), type(PropertyType::kFloat4) 
	{ _float[0] = value.x; _float[1] = value.z; _float[2] = value.y; _float[3] = value.w; }

	string name;
	PropertyType::Enum type;
	union {
		int _int;
		float _float[4];
	};
};

struct Material {
	Material(const string &name) : name(name) {}
	string name;
	std::vector<MaterialProperty> properties;
};