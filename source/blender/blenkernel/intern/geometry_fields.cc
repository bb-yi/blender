/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cstdlib>

#include "BLI_array_utils.hh"
#include "BLI_generic_array.hh"
#include "BLI_hash_mm3.hh"
#include "BLI_implicit_sharing_ptr.hh"
#include "BLI_memory_cache.hh"
#include "BLI_memory_counter.hh"
#include "BLI_mutex.hh"
#include "BLI_timeit.hh"
#include "BLI_unique_hash.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_storage.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"

#include "FN_multi_function_registry.hh"

#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "BLT_translation.hh"
#include "CLG_log.h"

#include <fmt/format.h>

namespace blender {

namespace bke {

MeshFieldContext::MeshFieldContext(const Mesh &mesh, const AttrDomain domain)
    : mesh_(mesh), domain_(domain)
{
  BLI_assert(mesh.attributes().domain_supported(domain_));
}

CurvesFieldContext::CurvesFieldContext(const CurvesGeometry &curves, const AttrDomain domain)
    : curves_(curves), domain_(domain)
{
  BLI_assert(curves.attributes().domain_supported(domain));
}

CurvesFieldContext::CurvesFieldContext(const Curves &curves_id, const AttrDomain domain)
    : CurvesFieldContext(curves_id.geometry.wrap(), domain)
{
  curves_id_ = &curves_id;
}

GVArray GreasePencilLayerFieldContext::get_varray_for_input(const fn::FieldInput &field_input,
                                                            const IndexMask &mask,
                                                            ResourceScope &scope) const
{
  if (const CurvesFieldInput *curves_field_input = dynamic_cast<const CurvesFieldInput *>(
          &field_input))
  {
    if (const bke::greasepencil::Drawing *drawing = this->grease_pencil().get_eval_drawing(
            this->grease_pencil().layer(this->layer_index())))
    {
      if (drawing->strokes().attributes().domain_supported(this->domain())) {
        const CurvesFieldContext context{drawing->strokes(), this->domain()};
        return curves_field_input->get_varray_for_context(context, mask, scope);
      }
    }
    return {};
  }
  return field_input.get_varray_for_context(*this, mask, scope);
}

GeometryFieldContext::GeometryFieldContext(const GeometryFieldContext &other,
                                           const AttrDomain domain)
    : geometry_(other.geometry_),
      type_(other.type_),
      domain_(domain),
      curves_id_(other.curves_id_),
      grease_pencil_layer_index_(other.grease_pencil_layer_index_)
{
}

GeometryFieldContext::GeometryFieldContext(const void *geometry,
                                           const GeometryComponent::Type type,
                                           const AttrDomain domain,
                                           const int grease_pencil_layer_index)
    : geometry_(geometry),
      type_(type),
      domain_(domain),
      grease_pencil_layer_index_(grease_pencil_layer_index)
{
  BLI_assert(ELEM(type,
                  GeometryComponent::Type::Mesh,
                  GeometryComponent::Type::Curve,
                  GeometryComponent::Type::PointCloud,
                  GeometryComponent::Type::GreasePencil,
                  GeometryComponent::Type::Instance));
}

GeometryFieldContext::GeometryFieldContext(const GeometryComponent &component,
                                           const AttrDomain domain)
    : type_(component.type()), domain_(domain)
{
  switch (component.type()) {
    case GeometryComponent::Type::Mesh: {
      const MeshComponent &mesh_component = static_cast<const MeshComponent &>(component);
      geometry_ = mesh_component.get();
      break;
    }
    case GeometryComponent::Type::Curve: {
      const CurveComponent &curve_component = static_cast<const CurveComponent &>(component);
      const Curves *curves = curve_component.get();
      geometry_ = curves ? &curves->geometry.wrap() : nullptr;
      curves_id_ = curve_component.get();
      break;
    }
    case GeometryComponent::Type::PointCloud: {
      const PointCloudComponent &pointcloud_component = static_cast<const PointCloudComponent &>(
          component);
      geometry_ = pointcloud_component.get();
      break;
    }
    case GeometryComponent::Type::GreasePencil: {
      const GreasePencilComponent &grease_pencil_component =
          static_cast<const GreasePencilComponent &>(component);
      geometry_ = grease_pencil_component.get();
      /* Need to use another constructor for other domains. */
      BLI_assert(domain == AttrDomain::Layer);
      break;
    }
    case GeometryComponent::Type::Instance: {
      const InstancesComponent &instances_component = static_cast<const InstancesComponent &>(
          component);
      geometry_ = instances_component.get();
      break;
    }
    case GeometryComponent::Type::Volume:
    case GeometryComponent::Type::Edit:
      BLI_assert_unreachable();
      break;
  }
}

GeometryFieldContext::GeometryFieldContext(const Mesh &mesh, AttrDomain domain)
    : geometry_(&mesh), type_(GeometryComponent::Type::Mesh), domain_(domain)
{
}
GeometryFieldContext::GeometryFieldContext(const CurvesGeometry &curves, AttrDomain domain)
    : geometry_(&curves), type_(GeometryComponent::Type::Curve), domain_(domain)
{
}
GeometryFieldContext::GeometryFieldContext(const Curves &curves_id, AttrDomain domain)
    : geometry_(&curves_id.geometry.wrap()),
      type_(GeometryComponent::Type::Curve),
      domain_(domain),
      curves_id_(&curves_id)
{
}
GeometryFieldContext::GeometryFieldContext(const PointCloud &points)
    : geometry_(&points), type_(GeometryComponent::Type::PointCloud), domain_(AttrDomain::Point)
{
}
GeometryFieldContext::GeometryFieldContext(const GreasePencil &grease_pencil)
    : geometry_(&grease_pencil),
      type_(GeometryComponent::Type::GreasePencil),
      domain_(AttrDomain::Layer)
{
}
GeometryFieldContext::GeometryFieldContext(const GreasePencil &grease_pencil,
                                           const AttrDomain domain,
                                           const int layer_index)
    : geometry_(&grease_pencil),
      type_(GeometryComponent::Type::GreasePencil),
      domain_(domain),
      grease_pencil_layer_index_(layer_index)
{
}
GeometryFieldContext::GeometryFieldContext(const Instances &instances)
    : geometry_(&instances),
      type_(GeometryComponent::Type::Instance),
      domain_(AttrDomain::Instance)
{
}

std::optional<AttributeAccessor> GeometryFieldContext::attributes() const
{
  if (const Mesh *mesh = this->mesh()) {
    return mesh->attributes();
  }
  if (const CurvesGeometry *curves = this->curves()) {
    return curves->attributes();
  }
  if (const PointCloud *pointcloud = this->pointcloud()) {
    return pointcloud->attributes();
  }
  if (const GreasePencil *grease_pencil = this->grease_pencil()) {
    if (domain_ == AttrDomain::Layer) {
      return grease_pencil->attributes();
    }
    if (const greasepencil::Drawing *drawing = grease_pencil->get_eval_drawing(
            grease_pencil->layer(grease_pencil_layer_index_)))
    {
      return drawing->strokes().attributes();
    }
  }
  if (const Instances *instances = this->instances()) {
    return instances->attributes();
  }
  return {};
}

const Mesh *GeometryFieldContext::mesh() const
{
  return this->type() == GeometryComponent::Type::Mesh ? static_cast<const Mesh *>(geometry_) :
                                                         nullptr;
}
const CurvesGeometry *GeometryFieldContext::curves() const
{
  return this->type() == GeometryComponent::Type::Curve ?
             static_cast<const CurvesGeometry *>(geometry_) :
             nullptr;
}
const PointCloud *GeometryFieldContext::pointcloud() const
{
  return this->type() == GeometryComponent::Type::PointCloud ?
             static_cast<const PointCloud *>(geometry_) :
             nullptr;
}
const GreasePencil *GeometryFieldContext::grease_pencil() const
{
  return this->type() == GeometryComponent::Type::GreasePencil ?
             static_cast<const GreasePencil *>(geometry_) :
             nullptr;
}
const greasepencil::Drawing *GeometryFieldContext::grease_pencil_layer_drawing() const
{
  if (!(this->type() == GeometryComponent::Type::GreasePencil) ||
      !ELEM(domain_, AttrDomain::Curve, AttrDomain::Point))
  {
    return nullptr;
  }
  return this->grease_pencil()->get_eval_drawing(
      this->grease_pencil()->layer(this->grease_pencil_layer_index_));
}
const CurvesGeometry *GeometryFieldContext::curves_or_strokes() const
{
  if (const CurvesGeometry *curves = this->curves()) {
    return curves;
  }
  if (const greasepencil::Drawing *drawing = this->grease_pencil_layer_drawing()) {
    return &drawing->strokes();
  }
  return nullptr;
}
const Curves *GeometryFieldContext::curves_id() const
{
  return curves_id_;
}
const Instances *GeometryFieldContext::instances() const
{
  return this->type() == GeometryComponent::Type::Instance ?
             static_cast<const Instances *>(geometry_) :
             nullptr;
}

static std::optional<GeometryFieldContext> try_get_geometry_field_context(
    const fn::FieldContext &context)
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context))
  {
    return *geometry_context;
  }
  if (const MeshFieldContext *mesh_context = dynamic_cast<const MeshFieldContext *>(&context)) {
    return GeometryFieldContext(mesh_context->mesh(), mesh_context->domain());
  }
  if (const CurvesFieldContext *curve_context = dynamic_cast<const CurvesFieldContext *>(&context))
  {
    if (const Curves *curves_id = curve_context->curves_id()) {
      return GeometryFieldContext(*curves_id, curve_context->domain());
    }
    return GeometryFieldContext(curve_context->curves(), curve_context->domain());
  }
  if (const PointCloudFieldContext *point_context = dynamic_cast<const PointCloudFieldContext *>(
          &context))
  {
    return GeometryFieldContext(point_context->pointcloud());
  }
  if (const GreasePencilFieldContext *grease_pencil_context =
          dynamic_cast<const GreasePencilFieldContext *>(&context))
  {
    return GeometryFieldContext(grease_pencil_context->grease_pencil());
  }
  if (const GreasePencilLayerFieldContext *grease_pencil_context =
          dynamic_cast<const GreasePencilLayerFieldContext *>(&context))
  {
    return GeometryFieldContext(grease_pencil_context->grease_pencil(),
                                grease_pencil_context->domain(),
                                grease_pencil_context->layer_index());
  }
  if (const InstancesFieldContext *instances_context = dynamic_cast<const InstancesFieldContext *>(
          &context))
  {
    return GeometryFieldContext(instances_context->instances());
  }
  return std::nullopt;
}

GVArray GeometryFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                                   const IndexMask &mask,
                                                   ResourceScope & /*scope*/) const
{
  if (const std::optional<GeometryFieldContext> geometry_context = try_get_geometry_field_context(
          context))
  {
    return this->get_varray_for_context(*geometry_context, mask);
  }
  return {};
}

std::optional<AttrDomain> GeometryFieldInput::preferred_domain(
    const GeometryComponent & /*component*/) const
{
  return std::nullopt;
}

NativeFieldDomain GeometryFieldInput::native_domain_info(
    const GeometryComponent & /*component*/) const
{
  return NativeFieldDomain::None();
}

GVArray MeshFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                               const IndexMask &mask,
                                               ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context))
  {
    if (const Mesh *mesh = geometry_context->mesh()) {
      return this->get_varray_for_context(*mesh, geometry_context->domain(), mask);
    }
  }
  if (const MeshFieldContext *mesh_context = dynamic_cast<const MeshFieldContext *>(&context)) {
    return this->get_varray_for_context(mesh_context->mesh(), mesh_context->domain(), mask);
  }
  return {};
}

std::optional<AttrDomain> MeshFieldInput::preferred_domain(const Mesh & /*mesh*/) const
{
  return std::nullopt;
}

NativeFieldDomain MeshFieldInput::native_domain_info(const Mesh & /*mesh*/) const
{
  return NativeFieldDomain::None();
}

GVArray CurvesFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                                 const IndexMask &mask,
                                                 ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context))
  {
    if (const CurvesGeometry *curves = geometry_context->curves_or_strokes()) {
      return this->get_varray_for_context(*curves, geometry_context->domain(), mask);
    }
  }
  if (const CurvesFieldContext *curves_context = dynamic_cast<const CurvesFieldContext *>(
          &context))
  {
    return this->get_varray_for_context(curves_context->curves(), curves_context->domain(), mask);
  }
  return {};
}

std::optional<AttrDomain> CurvesFieldInput::preferred_domain(
    const CurvesGeometry & /*curves*/) const
{
  return std::nullopt;
}

GVArray PointCloudFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                                     const IndexMask &mask,
                                                     ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context))
  {
    if (const PointCloud *pointcloud = geometry_context->pointcloud()) {
      return this->get_varray_for_context(*pointcloud, mask);
    }
  }
  if (const PointCloudFieldContext *point_context = dynamic_cast<const PointCloudFieldContext *>(
          &context))
  {
    return this->get_varray_for_context(point_context->pointcloud(), mask);
  }
  return {};
}

GVArray InstancesFieldInput::get_varray_for_context(const fn::FieldContext &context,
                                                    const IndexMask &mask,
                                                    ResourceScope & /*scope*/) const
{
  if (const GeometryFieldContext *geometry_context = dynamic_cast<const GeometryFieldContext *>(
          &context))
  {
    if (const Instances *instances = geometry_context->instances()) {
      return this->get_varray_for_context(*instances, mask);
    }
  }
  if (const InstancesFieldContext *instances_context = dynamic_cast<const InstancesFieldContext *>(
          &context))
  {
    return this->get_varray_for_context(instances_context->instances(), mask);
  }
  return {};
}

GVArray AttributeFieldInput::get_varray_for_context(const GeometryFieldContext &context,
                                                    const IndexMask & /*mask*/) const
{
  const bke::AttrType data_type = cpp_type_to_attribute_type(*type_);
  const AttrDomain domain = context.domain();
  if (const GreasePencil *grease_pencil = context.grease_pencil()) {
    const AttributeAccessor layer_attributes = grease_pencil->attributes();
    if (domain == AttrDomain::Layer) {
      return *layer_attributes.lookup(name_, data_type);
    }
    if (ELEM(domain, AttrDomain::Point, AttrDomain::Curve)) {
      const int layer_index = context.grease_pencil_layer_index();
      const AttributeAccessor curves_attributes = *context.attributes();
      if (const GAttributeReader reader = curves_attributes.lookup(name_, domain, data_type)) {
        return *reader;
      }
      /* Lookup attribute on the layer domain if it does not exist on points or curves. */
      if (const GAttributeReader reader = layer_attributes.lookup(name_)) {
        const CPPType &cpp_type = reader.varray.type();
        BUFFER_FOR_CPP_TYPE_VALUE(cpp_type, value);
        BLI_SCOPED_DEFER([&]() { cpp_type.destruct(value); });
        reader.varray.get_to_uninitialized(layer_index, value);
        const int domain_size = curves_attributes.domain_size(domain);
        return GVArray::from_single(cpp_type, domain_size, value);
      }
    }
  }
  else if (context.domain() == bke::AttrDomain::Instance && name_ == "position") {
    /* Special case for "position" which is no longer an attribute on instances. */
    return bke::instance_position_varray(*context.instances());
  }
  else if (auto attributes = context.attributes()) {
    return *attributes->lookup(name_, domain, data_type);
  }

  return {};
}

GVArray AttributeExistsFieldInput::get_varray_for_context(const bke::GeometryFieldContext &context,
                                                          const IndexMask & /*mask*/) const
{
  const AttrDomain domain = context.domain();
  if (context.type() == GeometryComponent::Type::GreasePencil) {
    const AttributeAccessor layer_attributes = context.grease_pencil()->attributes();
    if (context.domain() == AttrDomain::Layer) {
      const bool exists = layer_attributes.contains(name_);
      const int domain_size = layer_attributes.domain_size(AttrDomain::Layer);
      return VArray<bool>::from_single(exists, domain_size);
    }
    const greasepencil::Drawing *drawing = context.grease_pencil_layer_drawing();
    const AttributeAccessor curve_attributes = drawing->strokes().attributes();
    const bool exists = layer_attributes.contains(name_) || curve_attributes.contains(name_);
    const int domain_size = curve_attributes.domain_size(domain);
    return VArray<bool>::from_single(exists, domain_size);
  }
  const bool exists = context.attributes()->contains(name_);
  const int domain_size = context.attributes()->domain_size(domain);
  return VArray<bool>::from_single(exists, domain_size);
}

void AttributeExistsFieldInput::hash_unique(UniqueHashBytes &hash,
                                            fn::FieldHashDeep & /*deep_hash_cache*/) const
{
  static constexpr int8_t id = 0;
  hash.add(&id);
  hash.data.extend(Span(name_.data(), name_.size()).cast<std::byte>());
}

NativeFieldDomain AttributeExistsFieldInput::native_domain_info(
    const GeometryComponent & /*component*/) const
{
  return NativeFieldDomain::Constant();
}

std::string AttributeFieldInput::socket_inspection_name() const
{
  if (socket_inspection_name_) {
    return *socket_inspection_name_;
  }
  return fmt::format(fmt::runtime(TIP_("\"{}\" attribute from geometry")), name_);
}

void AttributeFieldInput::hash_unique(UniqueHashBytes &hash,
                                      fn::FieldHashDeep & /*deep_hash_cache*/) const
{
  hash.data.extend(Span(name_.data(), name_.size()).cast<std::byte>());
  hash.add(type_);
}

std::optional<AttrDomain> AttributeFieldInput::preferred_domain(
    const GeometryComponent &component) const
{
  const std::optional<AttributeAccessor> attributes = component.attributes();
  if (!attributes.has_value()) {
    return std::nullopt;
  }
  const std::optional<AttributeMetaData> meta_data = attributes->lookup_meta_data(name_);
  if (!meta_data.has_value()) {
    return std::nullopt;
  }
  return meta_data->domain;
}

NativeFieldDomain AttributeFieldInput::native_domain_info(const GeometryComponent &component) const
{
  const std::optional<AttributeAccessor> attributes = component.attributes();
  if (!attributes.has_value()) {
    return NativeFieldDomain::Constant();
  }
  const std::optional<AttributeMetaData> meta_data = attributes->lookup_meta_data(name_);
  if (!meta_data.has_value()) {
    return NativeFieldDomain::Constant();
  }
  return NativeFieldDomain(NativeFieldDomain::Domain{meta_data->domain});
}

static StringRef get_random_id_attribute_name(const AttrDomain domain)
{
  switch (domain) {
    case AttrDomain::Point:
    case AttrDomain::Instance:
      return "id";
    default:
      return "";
  }
}

GVArray IDAttributeFieldInput::get_varray_for_context(const GeometryFieldContext &context,
                                                      const IndexMask &mask) const
{

  const StringRef name = get_random_id_attribute_name(context.domain());
  if (auto attributes = context.attributes()) {
    if (GVArray attribute = *attributes->lookup<int>(name, context.domain())) {
      return attribute;
    }
  }

  /* Use the index as the fallback if no random ID attribute exists. */
  return fn::IndexFieldInput::get_index_varray(mask);
}

std::string IDAttributeFieldInput::socket_inspection_name() const
{
  return TIP_("ID / Index");
}

void IDAttributeFieldInput::hash_unique(UniqueHashBytes &hash,
                                        fn::FieldHashDeep & /*deep_hash_cache*/) const
{
  static constexpr int8_t id = 0;
  hash.add(&id);
}

const fn::Field<int> &IDAttributeFieldInput::get_field()
{
  static const fn::Field<int> field = fn::Field<int>::from_input<IDAttributeFieldInput>();
  static const fn::Field<int> field_ref = fn::Field<int>::from_non_owning_ref(field);
  return field_ref;
}

GVArray NamedLayerSelectionFieldInput::get_varray_for_context(
    const bke::GeometryFieldContext &context, const IndexMask &mask) const
{
  using namespace bke::greasepencil;
  const AttrDomain domain = context.domain();
  if (!ELEM(domain, AttrDomain::Point, AttrDomain::Curve, AttrDomain::Layer)) {
    return {};
  }

  const GreasePencil &grease_pencil = *context.grease_pencil();
  if (!context.grease_pencil()) {
    return {};
  }

  auto layer_is_selected = [selection_name = StringRef(layer_name_),
                            &grease_pencil,
                            size = mask.min_array_size()](const int layer_i) {
    if (layer_i < 0 || layer_i >= grease_pencil.layers().size()) {
      return false;
    }
    const Layer &layer = grease_pencil.layer(layer_i);
    return layer.name() == selection_name;
  };

  if (ELEM(domain, AttrDomain::Point, AttrDomain::Curve)) {
    const int layer_i = context.grease_pencil_layer_index();
    const bool selected = layer_is_selected(layer_i);
    return VArray<bool>::from_single(selected, mask.min_array_size());
  }

  return VArray<bool>::from_func(mask.min_array_size(), layer_is_selected);
}

void NamedLayerSelectionFieldInput::hash_unique(UniqueHashBytes &hash,
                                                fn::FieldHashDeep & /*deep_hash_cache*/) const
{
  hash.data.extend(Span(layer_name_.data(), layer_name_.size()).cast<std::byte>());
  hash.add(type_);
}

std::optional<AttrDomain> NamedLayerSelectionFieldInput::preferred_domain(
    const bke::GeometryComponent & /*component*/) const
{
  return AttrDomain::Layer;
}

EvaluateAtIndexInput::EvaluateAtIndexInput(fn::Field<int> index_field,
                                           fn::GField value_field,
                                           AttrDomain value_field_domain)
    : bke::GeometryFieldInput(value_field.cpp_type(), "Evaluate at Index"),
      index_field_(std::move(index_field)),
      value_field_(std::move(value_field)),
      value_field_domain_(value_field_domain)
{
}

GVArray EvaluateAtIndexInput::get_varray_for_context(const bke::GeometryFieldContext &context,
                                                     const IndexMask &mask) const
{
  const std::optional<AttributeAccessor> attributes = context.attributes();
  if (!attributes) {
    return {};
  }

  const bke::GeometryFieldContext value_context{context, value_field_domain_};
  fn::FieldEvaluator value_evaluator{value_context, attributes->domain_size(value_field_domain_)};
  value_evaluator.add(value_field_);
  value_evaluator.evaluate();
  const GVArray &values = value_evaluator.get_evaluated(0);

  fn::FieldEvaluator index_evaluator{context, &mask};
  index_evaluator.add(index_field_);
  index_evaluator.evaluate();
  const VArraySpan<int> indices = index_evaluator.get_evaluated<int>(0);

  GArray<> dst_array(values.type(), mask.min_array_size());
  IndexMaskMemory memory;
  const IndexMask valid_mask = array_utils::indices_in_range(
      mask, indices, values.index_range(), memory);
  bke::attribute_math::gather(values, indices, valid_mask, dst_array);
  dst_array.type().value_initialize_indices(dst_array.data(), valid_mask.complement(mask, memory));
  return GVArray::from_garray(std::move(dst_array));
}

void EvaluateAtIndexInput::hash_unique(UniqueHashBytes &hash,
                                       fn::FieldHashDeep &deep_hash_cache) const
{
  static constexpr int8_t id = 0;
  hash.add(&id);
  hash.add(type_);
  hash.add(deep_hash_cache.ensure(index_field_));
  hash.add(deep_hash_cache.ensure(value_field_));
  hash.add(value_field_domain_);
}

static bool component_is_available(const GeometrySet &geometry,
                                   const GeometryComponent::Type type,
                                   const AttrDomain domain)
{
  if (!geometry.has(type)) {
    return false;
  }
  const GeometryComponent &component = *geometry.get_component(type);
  return component.attribute_domain_size(domain) != 0;
}

const GeometryComponent *SampleIndexFunction::find_source_component(const GeometrySet &geometry,
                                                                    const AttrDomain domain)
{
  /* Choose the other component based on a consistent order, rather than some more complicated
   * heuristic. This is the same order visible in the spreadsheet and used in the ray-cast node. */
  static const Array<GeometryComponent::Type> supported_types = {
      GeometryComponent::Type::Mesh,
      GeometryComponent::Type::PointCloud,
      GeometryComponent::Type::Curve,
      GeometryComponent::Type::Instance,
      GeometryComponent::Type::GreasePencil};
  for (const GeometryComponent::Type src_type : supported_types) {
    if (component_is_available(geometry, src_type, domain)) {
      return geometry.get_component(src_type);
    }
  }

  return nullptr;
}

SampleIndexFunction::SampleIndexFunction(GeometrySet geometry,
                                         fn::GField src_field,
                                         const AttrDomain domain)
    : src_geometry_(std::move(geometry)), src_field_(std::move(src_field)), domain_(domain)
{
  src_geometry_.ensure_owns_direct_data();

  mf::SignatureBuilder builder{"Sample Index", signature_};
  builder.single_input<int>("Index");
  builder.single_output("Value", src_field_.cpp_type());
  this->set_signature(&signature_);
}

void SampleIndexFunction::prepare_for_execution() const
{
  mutex_.ensure([&]() {
    const GeometryComponent *component = find_source_component(src_geometry_, domain_);
    if (component == nullptr) {
      return;
    }
    const int domain_num = component->attribute_domain_size(domain_);
    geometry_context_.emplace(GeometryFieldContext(*component, domain_));
    evaluator_ = std::make_unique<fn::FieldEvaluator>(*geometry_context_, domain_num);
    evaluator_->add(src_field_);
    evaluator_->evaluate();
    src_data_ = &evaluator_->get_evaluated(0);
  });
}

void SampleIndexFunction::call(const IndexMask &mask,
                               mf::Params params,
                               mf::Context /*context*/) const
{
  const VArraySpan<int> indices = params.readonly_single_input<int>(0, "Index");
  GMutableSpan dst = params.uninitialized_single_output(1, "Value");

  const CPPType &type = dst.type();
  if (src_data_ == nullptr) {
    type.value_initialize_indices(dst.data(), mask);
    return;
  }

  IndexMaskMemory memory;
  const IndexMask valid_mask = array_utils::indices_in_range(
      mask, indices, src_data_->index_range(), memory);
  bke::attribute_math::gather(*src_data_, indices, valid_mask, dst);
  dst.type().value_initialize_indices(dst.data(), valid_mask.complement(mask, memory));
}

void SampleIndexFunction::hash_unique(UniqueHashBytes &hash) const
{
  static constexpr int8_t id = 0;
  hash.add(&id);
  hash.add(find_source_component(src_geometry_, domain_));
  hash.add(domain_);
  fn::FieldHashDeep field_hash;
  hash.add(field_hash.ensure(src_field_));
}

EvaluateOnDomainInput::EvaluateOnDomainInput(fn::GField field, AttrDomain domain)
    : bke::GeometryFieldInput(field.cpp_type(), "Evaluate on Domain"),
      src_field_(std::move(field)),
      src_domain_(domain)
{
}

static CLG_LogRef DOMAIN_FIELD_LOG = {"nodes.scatter_field_cache"};

/* A pure numeric field can be shared between modifiers evaluating the same mesh arrays.
 * Retain only the versioned sharing header. The results own their data, and the mesh should still
 * be able to edit its own arrays without copying them for the cache. */
struct DomainFieldKey : GenericKey {
  struct Input {
    WeakImplicitSharingPtr owner;
    const void *data = nullptr;
    const CPPType *type = nullptr;
    int64_t size = 0;
    int64_t version = 0;
    AttrDomain domain = AttrDomain::Point;
    Vector<std::byte, 64> single_value;

    void set_shared(const ImplicitSharingInfo &sharing_info,
                    const void *data,
                    const int64_t size)
    {
      sharing_info.add_weak_user();
      this->owner = WeakImplicitSharingPtr(&sharing_info);
      this->data = data;
      this->size = size;
      this->version = sharing_info.version();
    }

    friend bool operator==(const Input &, const Input &) = default;
  };
  Vector<Input> inputs;
  Vector<std::byte, 256> expression;
  AttrDomain domain = AttrDomain::Point;
  int4 sizes;
  uint32_t expression_hash = 0;

  uint64_t hash() const override
  {
    uint64_t hash = get_default_hash(expression_hash, int(domain), sizes);
    for (const Input &input : inputs) {
      const uint32_t value_hash = input.single_value.is_empty() ?
                                      0 :
                                      BLI_hash_mm3(reinterpret_cast<const unsigned char *>(
                                                       input.single_value.data()),
                                                   input.single_value.size(),
                                                   0);
      const uint64_t storage_hash = get_default_hash(
          input.owner.get(), input.data, input.size, input.version);
      hash = get_default_hash(hash, storage_hash, int(input.domain), input.type, value_hash);
    }
    return hash;
  }
  bool equal_to(const GenericKey &key) const override
  {
    const auto *other = dynamic_cast<const DomainFieldKey *>(&key);
    return other && expression_hash == other->expression_hash && domain == other->domain &&
           sizes == other->sizes && expression == other->expression && inputs == other->inputs;
  }
  std::unique_ptr<GenericKey> to_storable() const override
  {
    return std::make_unique<DomainFieldKey>(*this);
  }
};

static bool is_cacheable_numeric_type(const CPPType &type)
{
  return type.is<bool>() || type.is<int>() || type.is<float>() || type.is<float2>() ||
         type.is<float3>() || type.is<float4>() || type.is<float4x4>();
}

struct DomainFieldExpression {
  enum class Token : uint8_t { Constant, Operation, Attribute, Domain, Index };
  static constexpr int max_nodes = 512;
  static constexpr int max_depth = 32;
  static constexpr int max_parameter_bytes = 512;

  UniqueHashBytes shape;
  Vector<std::byte, 64> parameters;
  VectorSet<StringRefNull> names;
  int nodes = 0;

  bool append(const fn::GFieldRef field, const int depth = 0)
  {
    if (++nodes > max_nodes || depth > max_depth || !is_cacheable_numeric_type(field.cpp_type())) {
      return false;
    }
    if (const auto *value = std::get_if<fn::GFieldRef::Value>(&field.variant())) {
      shape.add(Token::Constant);
      shape.add(value->type);
      if (parameters.size() + value->type->size > max_parameter_bytes) {
        return false;
      }
      parameters.extend(static_cast<const std::byte *>(value->value), value->type->size);
      return true;
    }
    if (const auto *op = std::get_if<fn::GFieldRef::MultiFn>(&field.variant())) {
      const mf::MultiFunction &function = op->node->multi_function();
      if (!mf::registry::contains(function)) {
        CLOG_TRACE(&DOMAIN_FIELD_LOG, "Unsupported operation: %s", function.name().c_str());
        return false;
      }
      shape.add(Token::Operation);
      shape.add(&function);
      shape.add(op->output_i);
      const Span<fn::GField> children = op->node->inputs();
      shape.add(children.size());
      return std::ranges::all_of(children, [&](const fn::GField &child) {
        return append(child, depth + 1);
      });
    }
    const fn::FieldInput &input = *std::get<fn::GFieldRef::Input>(field.variant()).node;
    if (const auto *attribute = dynamic_cast<const AttributeFieldInput *>(&input)) {
      shape.add(Token::Attribute);
      shape.add(&input.cpp_type());
      const StringRefNull name = attribute->attribute_name();
      shape.add(name.size());
      shape.data.extend(reinterpret_cast<const std::byte *>(name.data()), name.size());
      names.add(name);
      return true;
    }
    if (const auto *domain = dynamic_cast<const EvaluateOnDomainInput *>(&input)) {
      shape.add(Token::Domain);
      shape.add(domain->source_domain());
      bool success = true;
      domain->foreach_recursive_field([&](const fn::GField &child) {
        success &= append(child, depth + 1);
      });
      return success;
    }
    if (dynamic_cast<const fn::IndexFieldInput *>(&input)) {
      shape.add(Token::Index);
      return true;
    }
    CLOG_STR_TRACE(&DOMAIN_FIELD_LOG, "Unsupported field input");
    return false;
  }
};

struct SharedDomainField : memory_cache::CachedValue {
  struct Version {
    Vector<std::byte, 64> parameters;
    CacheMutex mutex;
    GVArray values;
  };

  static constexpr int max_versions = 8;
  const int64_t input_bytes;
  const int64_t value_bytes;
  const int64_t expression_bytes;
  mutable Mutex versions_mutex;
  mutable Vector<std::shared_ptr<Version>, max_versions> versions;

  SharedDomainField(const DomainFieldKey &key, const int64_t value_bytes)
      : input_bytes(key.inputs.size() * sizeof(DomainFieldKey::Input)),
        value_bytes(value_bytes),
        expression_bytes(key.expression.size())
  {
  }

  std::shared_ptr<Version> lookup(Span<std::byte> parameters) const
  {
    std::lock_guard lock{versions_mutex};
    for (const int i : versions.index_range()) {
      if (versions[i]->parameters.as_span() == parameters) {
        std::shared_ptr<Version> match = std::move(versions[i]);
        versions.remove(i);
        versions.append(match);
        return match;
      }
    }
    auto version = std::make_shared<Version>();
    version->parameters.extend(parameters);
    if (versions.size() == max_versions) {
      versions.remove(0);
    }
    versions.append(version);
    return version;
  }

  void count_memory(MemoryCounter &memory) const override
  {
    memory.add(sizeof(*this) + sizeof(DomainFieldKey) + expression_bytes + input_bytes +
               max_versions *
                   (sizeof(Version) + DomainFieldExpression::max_parameter_bytes + value_bytes));
  }
};

static std::optional<DomainFieldKey> mesh_field_key(const Mesh &mesh, const fn::GField &field,
                                                    const AttrDomain domain,
                                                    DomainFieldExpression &expression)
{
  static const bool enabled = [] {
    const char *value = std::getenv("BLENDER_GN_DOMAIN_CACHE");
    if (!value) {
      value = std::getenv("BLENDER_GN_SCATTER_CACHE");
    }
    return value == nullptr || !STREQ(value, "0");
  }();
  if (!enabled || mesh.corners_num < 4096 || !mesh.runtime->face_offsets_sharing_info ||
      (!field.cpp_type().is<bool>() && !field.cpp_type().is<float>())) {
    return std::nullopt;
  }
  if (!expression.append(fn::GFieldRef(field))) {
    return std::nullopt;
  }
  DomainFieldKey key;
  /* Domain adaptation uses the actual topology arrays. The attribute accessor can return a
   * vertex group of the same name ahead of those arrays, so use the raw storage here. */
  const AttributeStorage &storage = mesh.attribute_storage.wrap();
  const auto add_topology = [&](const StringRef name) {
    const Attribute *attribute = storage.lookup(name);
    const auto *array = attribute ? std::get_if<Attribute::ArrayData>(&attribute->data()) :
                                    nullptr;
    if (!array || !array->sharing_info) {
      return false;
    }
    key.inputs.append_as();
    DomainFieldKey::Input &input = key.inputs.last();
    input.type = &attribute_type_to_cpp_type(attribute->data_type());
    input.domain = attribute->domain();
    input.set_shared(*array->sharing_info, array->data, array->size);
    return true;
  };
  if (!add_topology(".edge_verts") || !add_topology(".corner_vert") ||
      !add_topology(".corner_edge"))
  {
    return std::nullopt;
  }
  key.inputs.append_as();
  DomainFieldKey::Input &offsets = key.inputs.last();
  offsets.type = &CPPType::get<int>();
  offsets.domain = AttrDomain::Face;
  offsets.set_shared(
      *mesh.runtime->face_offsets_sharing_info, mesh.face_offset_indices, int64_t(mesh.faces_num) + 1);

  const AttributeAccessor attributes = mesh.attributes();
  for (const StringRefNull name : expression.names) {
    const GAttributeReader reader = attributes.lookup(name);
    key.inputs.append_as();
    DomainFieldKey::Input &input = key.inputs.last();
    if (!reader) {
      continue;
    }
    input.type = &reader.varray.type();
    input.domain = reader.domain;
    if (reader.sharing_info && reader.varray.is_span()) {
      input.set_shared(
          *reader.sharing_info, reader.varray.get_internal_span().data(), reader.varray.size());
    }
    else if (reader.varray.is_single() && is_cacheable_numeric_type(*input.type)) {
      BUFFER_FOR_CPP_TYPE_VALUE(*input.type, buffer);
      reader.varray.get_internal_single_to_uninitialized(buffer);
      input.single_value.extend(static_cast<const std::byte *>(buffer), input.type->size);
    }
    else {
      CLOG_TRACE(&DOMAIN_FIELD_LOG, "Unsupported attribute storage: %s", name.c_str());
      return std::nullopt;
    }
  }
  key.expression = std::move(expression.shape.data);
  key.expression_hash = BLI_hash_mm3(
      reinterpret_cast<const unsigned char *>(key.expression.data()), key.expression.size(), 0);
  key.domain = domain;
  key.sizes = int4(mesh.verts_num, mesh.edges_num, mesh.faces_num, mesh.corners_num);
  return key;
}

GVArray EvaluateOnDomainInput::get_varray_for_context(const bke::GeometryFieldContext &context,
                                                      const IndexMask & /*mask*/) const
{
  const AttrDomain dst_domain = context.domain();
  const int dst_domain_size = context.attributes()->domain_size(dst_domain);
  const CPPType &cpp_type = src_field_.cpp_type();

  if (context.type() == GeometryComponent::Type::GreasePencil &&
      (src_domain_ == AttrDomain::Layer) != (dst_domain == AttrDomain::Layer))
  {
    /* Evaluate field just for the current layer. */
    if (src_domain_ == AttrDomain::Layer) {
      const bke::GeometryFieldContext src_domain_context{context, AttrDomain::Layer};
      const int layer_index = context.grease_pencil_layer_index();

      const IndexMask single_layer_mask = IndexRange(layer_index, 1);
      fn::FieldEvaluator value_evaluator{src_domain_context, &single_layer_mask};
      value_evaluator.add(src_field_);
      value_evaluator.evaluate();

      const GVArray &values = value_evaluator.get_evaluated(0);

      BUFFER_FOR_CPP_TYPE_VALUE(cpp_type, value);
      BLI_SCOPED_DEFER([&]() { cpp_type.destruct(value); });
      values.get_to_uninitialized(layer_index, value);
      return GVArray::from_single(cpp_type, dst_domain_size, value);
    }
    /* We don't adapt from curve to layer domain currently. */
    return GVArray::from_single_default(cpp_type, dst_domain_size);
  }

  const bke::AttributeAccessor attributes = *context.attributes();

  const bke::GeometryFieldContext other_domain_context{context, src_domain_};
  const int64_t src_domain_size = attributes.domain_size(src_domain_);
  const auto adapt = [&](const GVArray &values) {
    const bool measure = dst_domain == AttrDomain::Corner &&
                         ELEM(src_domain_, AttrDomain::Edge, AttrDomain::Face) &&
                         cpp_type.is<bool>() && dst_domain_size >= 4096 &&
                         CLOG_CHECK(&DOMAIN_FIELD_LOG, CLG_LEVEL_INFO);
    const auto start = measure ? timeit::Clock::now() : timeit::TimePoint{};
    GVArray result = attributes.adapt_domain(values, src_domain_, dst_domain);
    if (measure) {
      CLOG_INFO(&DOMAIN_FIELD_LOG,
                "bool_corner source=%d size=%lld span=%d adapt_ms=%.3f",
                int(src_domain_),
                static_cast<long long>(dst_domain_size),
                int(values.is_span()),
                std::chrono::duration<double, std::milli>(timeit::Clock::now() - start).count());
    }
    return result;
  };
  auto evaluate = [&] {
    GArray<> values(cpp_type, src_domain_size);
    fn::FieldEvaluator value_evaluator{other_domain_context, src_domain_size};
    value_evaluator.add_with_destination(src_field_, values.as_mutable_span());
    value_evaluator.evaluate();
    return GVArray::from_garray(std::move(values));
  };
  if (const Mesh *mesh = context.mesh()) {
    const bool trace = CLOG_CHECK(&DOMAIN_FIELD_LOG, CLG_LEVEL_INFO);
    const auto lookup_start = trace ? timeit::Clock::now() : timeit::TimePoint{};
    DomainFieldExpression expression;
    if (auto key = mesh_field_key(*mesh, src_field_, src_domain_, expression)) {
      const auto cached = memory_cache::get<SharedDomainField>(*key, [&] {
        return std::make_unique<SharedDomainField>(*key, src_domain_size * cpp_type.size);
      });
      const auto version = cached->lookup(expression.parameters);
      const auto lookup_end = trace ? timeit::Clock::now() : timeit::TimePoint{};
      bool computed = false;
      double compute_ms = 0.0;
      version->mutex.ensure([&] {
        const auto compute_start = trace ? timeit::Clock::now() : timeit::TimePoint{};
        version->values = evaluate();
        computed = true;
        if (trace) {
          compute_ms = std::chrono::duration<double, std::milli>(timeit::Clock::now() -
                                                                compute_start)
                           .count();
        }
      });
      if (trace) {
        const double lookup_ms = std::chrono::duration<double, std::milli>(lookup_end -
                                                                           lookup_start)
                                     .count();
        const double ensure_ms = std::chrono::duration<double, std::milli>(timeit::Clock::now() -
                                                                           lookup_end)
                                     .count();
        const uint32_t parameter_hash = expression.parameters.is_empty() ?
                                            0 :
                                            BLI_hash_mm3(reinterpret_cast<const unsigned char *>(
                                                             expression.parameters.data()),
                                                         expression.parameters.size(),
                                                         0);
        CLOG_INFO(&DOMAIN_FIELD_LOG,
                  "domain=%d size=%lld expression=%08x storage=%llx parameters=%08x "
                  "computed=%d lookup_ms=%.3f ensure_ms=%.3f compute_ms=%.3f",
                  int(src_domain_),
                  static_cast<long long>(src_domain_size),
                  key->expression_hash,
                  static_cast<unsigned long long>(key->hash()),
                  parameter_hash,
                  int(computed),
                  lookup_ms,
                  ensure_ms,
                  compute_ms);
      }
      return adapt(version->values);
    }
  }
  return adapt(evaluate());
}

void EvaluateOnDomainInput::hash_unique(UniqueHashBytes &hash,
                                        fn::FieldHashDeep &deep_hash_cache) const
{
  static constexpr int8_t id = 0;
  hash.add(&id);
  hash.add(deep_hash_cache.ensure(src_field_));
  hash.add(src_domain_);
}

void EvaluateOnDomainInput::foreach_recursive_field(FunctionRef<void(const fn::GField &)> fn) const
{
  fn(src_field_);
}

std::optional<AttrDomain> EvaluateOnDomainInput::preferred_domain(
    const GeometryComponent & /*component*/) const
{
  return src_domain_;
}

NativeFieldDomain EvaluateOnDomainInput::native_domain_info(
    const GeometryComponent & /*component*/) const
{
  return bke::NativeFieldDomain::Domain{src_domain_};
}

}  // namespace bke

/* -------------------------------------------------------------------- */
/** \name Mesh and Curve Normals Field Input
 * \{ */

namespace bke {

GVArray NormalFieldInput::get_varray_for_context(const GeometryFieldContext &context,
                                                 const IndexMask &mask) const
{
  if (const Mesh *mesh = context.mesh()) {
    return mesh_normals_varray(
        *mesh, mask, context.domain(), legacy_corner_normals_, true_normals_);
  }
  if (const CurvesGeometry *curves = context.curves_or_strokes()) {
    return curve_normals_varray(*curves, context.domain());
  }
  return {};
}

std::string NormalFieldInput::socket_inspection_name() const
{
  return true_normals_ ? TIP_("True Normal") : TIP_("Normal");
}

void NormalFieldInput::hash_unique(UniqueHashBytes &hash,
                                   fn::FieldHashDeep & /*deep_hash_cache*/) const
{
  static constexpr int8_t id = 0;
  hash.add(&id);
  hash.add(legacy_corner_normals_);
  hash.add(true_normals_);
}

NativeFieldDomain NormalFieldInput::native_domain_info(const GeometryComponent &component) const
{
  switch (component.type()) {
    case GeometryComponent::Type::Mesh: {
      if (const Mesh *mesh = static_cast<const MeshComponent &>(component).get()) {
        switch (mesh->normals_domain()) {
          case MeshNormalDomain::Face:
            return bke::NativeFieldDomain::Domain{AttrDomain::Face};
          case MeshNormalDomain::Point:
            return bke::NativeFieldDomain::Domain{AttrDomain::Point};
          case MeshNormalDomain::Corner:
            return bke::NativeFieldDomain::Domain{AttrDomain::Corner};
        }
      }
      break;
    }
    case GeometryComponent::Type::Curve:
      return bke::NativeFieldDomain::Domain{AttrDomain::Point};
    default:
      return bke::NativeFieldDomain::Constant();
  }
  return bke::NativeFieldDomain::Constant();
}

const fn::Field<float3> &NormalFieldInput::get_field()
{
  static const fn::Field<float3> field = fn::Field<float3>::from_input<NormalFieldInput>();
  static const fn::Field<float3> field_ref = fn::Field<float3>::from_non_owning_ref(field);
  return field_ref;
}

static std::optional<StringRefNull> try_get_field_direct_attribute_id(const fn::GField &any_field)
{
  if (const auto *attribute_input = any_field.get_input_if<AttributeFieldInput>()) {
    return attribute_input->attribute_name();
  }
  return {};
}

static bool attribute_kind_matches(const AttributeMetaData meta_data,
                                   const AttrDomain domain,
                                   const bke::AttrType data_type)
{
  return meta_data.domain == domain && meta_data.data_type == data_type;
}

/**
 * Some fields reference attributes directly. When the referenced attribute has the requested type
 * and domain, use implicit sharing to avoid duplication when creating the captured attribute.
 */
static bool try_add_shared_field_attribute(MutableAttributeAccessor attributes,
                                           const StringRef id_to_create,
                                           const AttrDomain domain,
                                           const fn::GField &field)
{
  const std::optional<StringRef> field_id = try_get_field_direct_attribute_id(field);
  if (!field_id) {
    return false;
  }
  const std::optional<AttributeMetaData> meta_data = attributes.lookup_meta_data(*field_id);
  if (!meta_data) {
    return false;
  }
  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(field.cpp_type());
  if (!attribute_kind_matches(*meta_data, domain, data_type)) {
    /* Avoid costly domain and type interpolation, which would make sharing impossible. */
    return false;
  }
  const GAttributeReader attribute = attributes.lookup(*field_id, domain, data_type);
  if (!attribute.sharing_info || !attribute.varray.is_span()) {
    return false;
  }
  const AttributeInitShared init(attribute.varray.get_internal_span().data(),
                                 *attribute.sharing_info);
  return attributes.add_override(id_to_create, domain, data_type, init);
}

static bool attribute_data_matches_varray(const GAttributeReader &attribute,
                                          const CommonVArrayInfo &varray_info)
{
  if (varray_info.type != CommonVArrayInfo::Type::Span) {
    return false;
  }
  const CommonVArrayInfo attribute_info = attribute.varray.common_info();
  if (attribute_info.type != CommonVArrayInfo::Type::Span) {
    return false;
  }
  return varray_info.data == attribute_info.data;
}

static bool try_assign_single_value(MutableAttributeAccessor &attributes,
                                    const StringRef name,
                                    const GPointer value)
{
  return attributes.assign_data(name, AttributeInitValue(value));
}

static void initialize_new_data(MutableAttributeAccessor &attributes,
                                const AttrDomain domain,
                                const int domain_size,
                                const StringRef name,
                                const CPPType &type,
                                const bke::AttrType data_type,
                                void *buffer)
{
  /* NOTE: It's unnecessary to fill the values for elements that will be selected and also set
   * during field evaluation. A future optimization could evaluate the selection separately and use
   * its inverse here. */

  if (attributes.is_builtin(name)) {
    if (const GPointer value = attributes.get_builtin_default(name)) {
      type.fill_construct_n(value.get(), buffer, domain_size);
      return;
    }
  }
  if (const GAttributeReader old_attribute = attributes.lookup(name, domain, data_type)) {
    old_attribute.varray.materialize(buffer);
    return;
  }
  type.fill_construct_n(type.default_value(), buffer, domain_size);
}

bool try_capture_fields_on_geometry(MutableAttributeAccessor attributes,
                                    const fn::FieldContext &field_context,
                                    const Span<StringRef> names,
                                    const AttrDomain domain,
                                    const fn::Field<bool> &selection,
                                    const Span<fn::GField> fields)
{
  BLI_assert(names.size() == fields.size());
  const int domain_size = attributes.domain_size(domain);
  if (domain_size == 0) {
    bool all_added = true;
    for (const int i : names.index_range()) {
      const bke::AttrType data_type = bke::cpp_type_to_attribute_type(fields[i].cpp_type());
      all_added &= attributes.add(names[i], domain, data_type, AttributeInitConstruct{});
    }
    return all_added;
  }

  fn::FieldEvaluator evaluator{field_context, domain_size};
  evaluator.set_selection(selection);

  const bool selection_is_full = !selection.depends_on_input() &&
                                 fn::evaluate_constant_field(selection);

  struct StoreResult {
    int input_index;
    int evaluator_index;
  };
  Vector<StoreResult> results_to_store;

  ResourceScope scope;

  struct AddResult {
    int input_index;
    struct Single {
      void *value;
    };
    struct Array {
      ImplicitSharingPtr<ImplicitSharedValue<GArray<>>> data;
    };
    std::variant<Single, Array> new_data;
  };
  Vector<AddResult> results_to_add;

  bool success = true;

  for (const int input_index : names.index_range()) {
    const StringRef name = names[input_index];
    const CPPType &type = fields[input_index].cpp_type();
    const bke::AttrType data_type = bke::cpp_type_to_attribute_type(type);

    /* Avoid adding or writing to builtin attributes with an incorrect type or domain. */
    if (const std::optional<AttributeDomainAndType> meta_data =
            attributes.get_builtin_domain_and_type(name))
    {
      if (*meta_data != AttributeDomainAndType{domain, data_type}) {
        success = false;
        continue;
      }
    }

    const AttributeValidator validator = attributes.lookup_validator(name);
    const fn::GField field = validator.validate_field_if_necessary(fields[input_index]);

    if (!validator && selection_is_full) {
      if (try_add_shared_field_attribute(attributes, name, domain, field)) {
        continue;
      }
    }

    /* We are writing to an attribute that exists already with the correct domain and type. */
    if (const GAttributeReader dst = attributes.lookup(name)) {
      if (dst.domain == domain && dst.varray.type() == field.cpp_type()) {
        const int evaluator_index = evaluator.add(field);
        results_to_store.append({input_index, evaluator_index});
        continue;
      }
    }

    if (field.depends_on_input() || !selection_is_full) {
      /* Could avoid allocating a new buffer if:
       * - The field does not depend on that attribute (we can't easily check for that yet). */
      auto *data = new ImplicitSharedValue<GArray<>>(type, domain_size, NoInitialization());
      void *buffer = data->data.data();
      if (!selection_is_full) {
        initialize_new_data(attributes, domain, domain_size, name, type, data_type, buffer);
      }

      GMutableSpan dst(type, buffer, domain_size);
      evaluator.add_with_destination(field, dst);
      results_to_add.append({input_index, AddResult::Array{ImplicitSharingPtr(data)}});
    }
    else {
      void *value = scope.allocate_owned(field.cpp_type());
      fn::evaluate_constant_field(field, value);
      results_to_add.append({input_index, AddResult::Single{value}});
    }
  }

  evaluator.evaluate();
  const IndexMask &mask = evaluator.get_evaluated_selection_as_mask();

  for (const StoreResult &result : results_to_store) {
    const StringRef name = names[result.input_index];
    const GVArray &result_data = evaluator.get_evaluated(result.evaluator_index);
    const CommonVArrayInfo info = result_data.common_info();
    if (selection_is_full) {
      if (info.type == CommonVArrayInfo::Type::Single) {
        if (try_assign_single_value(attributes, name, GPointer(result_data.type(), info.data))) {
          continue;
        }
      }
    }
    const GAttributeReader dst = attributes.lookup(name);
    if (!attribute_data_matches_varray(dst, info)) {
      GSpanAttributeWriter dst_mut = attributes.lookup_for_write_span(name);
      array_utils::copy(result_data, mask, dst_mut.span);
      dst_mut.finish();
    }
  }

  for (AddResult &result : results_to_add) {
    const StringRef name = names[result.input_index];
    attributes.remove(name);
    const CPPType &type = fields[result.input_index].cpp_type();
    const bke::AttrType data_type = bke::cpp_type_to_attribute_type(type);
    if (auto *array = std::get_if<AddResult::Array>(&result.new_data)) {
      const ImplicitSharedValue<GArray<>> &data = *array->data;
      if (!attributes.add(name, domain, data_type, AttributeInitShared(data.data.data(), data))) {
        /* If the name corresponds to a builtin attribute, removing the attribute might fail if
         * it's required, adding the attribute might fail if the domain or type is incorrect. */
        success = false;
      }
    }
    else {
      const auto value = std::get<AddResult::Single>(result.new_data);
      const AttributeInitValue init(GPointer(type, value.value));
      if (!attributes.add(name, domain, data_type, init)) {
        success = false;
      }
    }
  }

  return success;
}

bool try_capture_fields_on_geometry(GeometryComponent &component,
                                    const Span<StringRef> names,
                                    const AttrDomain domain,
                                    const fn::Field<bool> &selection,
                                    const Span<fn::GField> fields)
{
  const GeometryComponent::Type component_type = component.type();
  if (component_type == GeometryComponent::Type::GreasePencil &&
      ELEM(domain, AttrDomain::Point, AttrDomain::Curve))
  {
    /* Capture the field on every layer individually. */
    auto &grease_pencil_component = static_cast<GreasePencilComponent &>(component);
    GreasePencil *grease_pencil = grease_pencil_component.get_for_write();
    if (grease_pencil == nullptr) {
      return false;
    }
    bool any_success = false;
    threading::parallel_for(grease_pencil->layers().index_range(), 8, [&](const IndexRange range) {
      for (const int layer_index : range) {
        if (greasepencil::Drawing *drawing = grease_pencil->get_eval_drawing(
                grease_pencil->layer(layer_index)))
        {
          const GeometryFieldContext field_context{*grease_pencil, domain, layer_index};
          const bool success = try_capture_fields_on_geometry(
              drawing->strokes_for_write().attributes_for_write(),
              field_context,
              names,
              domain,
              selection,
              fields);
          if (success & !any_success) {
            any_success = true;
          }
        }
      }
    });
    return any_success;
  }
  if (component_type == GeometryComponent::Type::GreasePencil && domain != AttrDomain::Layer) {
    /* The remaining code only handles the layer domain for grease pencil geometries. */
    return false;
  }

  MutableAttributeAccessor attributes = *component.attributes_for_write();
  const GeometryFieldContext field_context{component, domain};
  return try_capture_fields_on_geometry(
      attributes, field_context, names, domain, selection, fields);
}

bool try_capture_fields_on_geometry(GeometryComponent &component,
                                    const Span<StringRef> names,
                                    const AttrDomain domain,
                                    const Span<fn::GField> fields)
{
  const fn::Field<bool> selection = fn::Field<bool>(true);
  return try_capture_fields_on_geometry(component, names, domain, selection, fields);
}

std::optional<AttrDomain> try_detect_native_field_domain(const GeometryComponent &component,
                                                         const fn::GField &field)
{
  const fn::FieldInputsPtr &field_inputs = field.field_inputs();
  if (!field_inputs) {
    return std::nullopt;
  }
  VectorSet<AttrDomain, 8> domains;
  for (const fn::FieldInput &field_input : field_inputs->inputs) {
    if (const auto *input = dynamic_cast<const GeometryFieldInput *>(&field_input)) {
      const NativeFieldDomain domain_info = input->native_domain_info(component);
      if (std::holds_alternative<NativeFieldDomain::None>(domain_info.variant)) {
        return std::nullopt;
      }
      if (const auto *value = std::get_if<NativeFieldDomain::Domain>(&domain_info.variant)) {
        domains.add(value->domain);
      }
    }
    if (component.type() == GeometryComponent::Type::Mesh) {
      if (const Mesh *mesh = static_cast<const MeshComponent &>(component).get()) {
        if (const auto *input = dynamic_cast<const MeshFieldInput *>(&field_input)) {
          const NativeFieldDomain domain_info = input->native_domain_info(*mesh);
          if (std::holds_alternative<NativeFieldDomain::None>(domain_info.variant)) {
            return std::nullopt;
          }
          if (const auto *value = std::get_if<NativeFieldDomain::Domain>(&domain_info.variant)) {
            domains.add(value->domain);
          }
        }
      }
    }
    return std::nullopt;
  }
  if (domains.size() != 1) {
    /* Any combination of domains means there is no particular native domain. */
    return std::nullopt;
  }
  return domains[0];
}

std::optional<AttrDomain> try_detect_field_domain(const GeometryComponent &component,
                                                  const fn::GField &field)
{
  const GeometryComponent::Type component_type = component.type();
  if (component_type == GeometryComponent::Type::PointCloud) {
    return AttrDomain::Point;
  }
  if (component_type == GeometryComponent::Type::GreasePencil) {
    return AttrDomain::Layer;
  }
  if (component_type == GeometryComponent::Type::Instance) {
    return AttrDomain::Instance;
  }
  const fn::FieldInputsPtr &field_inputs = field.field_inputs();
  if (!field_inputs) {
    return std::nullopt;
  }
  std::optional<AttrDomain> output_domain;
  auto handle_domain = [&](const std::optional<AttrDomain> domain) {
    if (!domain.has_value()) {
      return false;
    }
    if (output_domain.has_value()) {
      if (*output_domain != *domain) {
        return false;
      }
      return true;
    }
    output_domain = domain;
    return true;
  };
  if (component_type == GeometryComponent::Type::Mesh) {
    const MeshComponent &mesh_component = static_cast<const MeshComponent &>(component);
    const Mesh *mesh = mesh_component.get();
    if (mesh == nullptr) {
      return std::nullopt;
    }
    for (const fn::FieldInput &field_input : field_inputs->inputs) {
      if (const auto *geometry_field_input = dynamic_cast<const GeometryFieldInput *>(
              &field_input))
      {
        if (!handle_domain(geometry_field_input->preferred_domain(component))) {
          return std::nullopt;
        }
      }
      else if (const auto *mesh_field_input = dynamic_cast<const MeshFieldInput *>(&field_input)) {
        if (!handle_domain(mesh_field_input->preferred_domain(*mesh))) {
          return std::nullopt;
        }
      }
      else {
        return std::nullopt;
      }
    }
  }
  if (component_type == GeometryComponent::Type::Curve) {
    const CurveComponent &curve_component = static_cast<const CurveComponent &>(component);
    const Curves *curves = curve_component.get();
    if (curves == nullptr) {
      return std::nullopt;
    }
    for (const fn::FieldInput &field_input : field_inputs->inputs) {
      if (const auto *geometry_field_input = dynamic_cast<const GeometryFieldInput *>(
              &field_input))
      {
        if (!handle_domain(geometry_field_input->preferred_domain(component))) {
          return std::nullopt;
        }
      }
      else if (const auto *curves_field_input = dynamic_cast<const CurvesFieldInput *>(
                   &field_input))
      {
        if (!handle_domain(curves_field_input->preferred_domain(curves->geometry.wrap()))) {
          return std::nullopt;
        }
      }
      else {
        return std::nullopt;
      }
    }
  }
  return output_domain;
}

}  // namespace bke

/** \} */

}  // namespace blender
