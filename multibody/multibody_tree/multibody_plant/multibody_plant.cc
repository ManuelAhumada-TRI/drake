#include "drake/multibody/multibody_tree/multibody_plant/multibody_plant.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "drake/common/default_scalars.h"
#include "drake/common/drake_throw.h"
#include "drake/geometry/frame_id_vector.h"
#include "drake/geometry/frame_kinematics_vector.h"
#include "drake/geometry/geometry_frame.h"
#include "drake/geometry/geometry_instance.h"

namespace drake {
namespace multibody {
namespace multibody_plant {

// Helper macro to throw an exception within methods that should not be called
// post-finalize.
#define DRAKE_MBP_THROW_IF_FINALIZED() ThrowIfFinalized(__func__)

// Helper macro to throw an exception within methods that should not be called
// pre-finalize.
#define DRAKE_MBP_THROW_IF_NOT_FINALIZED() ThrowIfNotFinalized(__func__)

using geometry::FrameId;
using geometry::FrameIdVector;
using geometry::FramePoseVector;
using geometry::GeometryFrame;
using geometry::GeometryId;
using geometry::GeometryInstance;
using geometry::GeometrySystem;
using geometry::PenetrationAsPointPair;
using geometry::SourceId;
using systems::InputPortDescriptor;
using systems::OutputPort;
using systems::State;

using drake::multibody::MultibodyForces;
using drake::multibody::MultibodyTree;
using drake::multibody::MultibodyTreeContext;
using drake::multibody::PositionKinematicsCache;
using drake::multibody::SpatialAcceleration;
using drake::multibody::SpatialForce;
using drake::multibody::VelocityKinematicsCache;
using systems::BasicVector;
using systems::Context;
using systems::InputPortDescriptor;

template<typename T>
MultibodyPlant<T>::MultibodyPlant() :
    systems::LeafSystem<T>(systems::SystemTypeTag<
        drake::multibody::multibody_plant::MultibodyPlant>()) {
  model_ = std::make_unique<MultibodyTree<T>>();
}

template<typename T>
template<typename U>
MultibodyPlant<T>::MultibodyPlant(const MultibodyPlant<U>& other) {
  DRAKE_THROW_UNLESS(other.is_finalized());
  model_ = other.model_->template CloneToScalar<T>();
  // Copy of all members related with geometry registration.
  source_id_ = other.source_id_;
  body_index_to_frame_id_ = other.body_index_to_frame_id_;
  geometry_id_to_body_index_ = other.geometry_id_to_body_index_;
  geometry_id_to_visual_index_ = other.geometry_id_to_visual_index_;
  // MultibodyTree::CloneToScalar() already called MultibodyTree::Finalize() on
  // the new MultibodyTree on U. Therefore we only Finilize the plant's
  // internals (and not the MultibodyTree).
  FinalizePlantOnly();
}

template <typename T>
geometry::SourceId MultibodyPlant<T>::RegisterAsSourceForGeometrySystem(
    geometry::GeometrySystem<T>* geometry_system) {
  DRAKE_THROW_UNLESS(geometry_system != nullptr);
  DRAKE_THROW_UNLESS(!geometry_source_is_registered());
  source_id_ = geometry_system->RegisterSource();
  // Save the GS pointer so that on later geometry registrations we can verify
  // the user is making calls on the same GS instance. Only used for that
  // purpose, it gets nullified at Finalize().
  geometry_system_ = geometry_system;
  return source_id_.value();
}

template<typename T>
void MultibodyPlant<T>::RegisterVisualGeometry(
    const Body<T>& body,
    const Isometry3<double>& X_BG, const geometry::Shape& shape,
    geometry::GeometrySystem<T>* geometry_system) {
  DRAKE_MBP_THROW_IF_FINALIZED();
  DRAKE_THROW_UNLESS(geometry_system != nullptr);
  DRAKE_THROW_UNLESS(geometry_source_is_registered());
  if (geometry_system != geometry_system_) {
    throw std::logic_error(
        "Geometry registration calls must be performed on the SAME instance of "
        "GeometrySystem used on the first call to "
        "RegisterAsSourceForGeometrySystem()");
  }
  GeometryId id;
  // TODO(amcastro-tri): Consider doing this after finalize so that we can
  // register anchored geometry on ANY body welded to the world.
  if (body.index() == world_index()) {
    id = RegisterAnchoredGeometry(X_BG, shape, geometry_system);
  } else {
    id = RegisterGeometry(body, X_BG, shape, geometry_system);
  }
  const int visual_index = geometry_id_to_visual_index_.size();
  geometry_id_to_visual_index_[id] = visual_index;
}

template<typename T>
void MultibodyPlant<T>::RegisterCollisionGeometry(
    const Body<T>& body,
    const Isometry3<double>& X_BG, const geometry::Shape& shape,
    geometry::GeometrySystem<T>* geometry_system) {
  DRAKE_MBP_THROW_IF_FINALIZED();
  DRAKE_THROW_UNLESS(geometry_system != nullptr);
  DRAKE_THROW_UNLESS(geometry_source_is_registered());
  if (geometry_system != geometry_system_) {
    throw std::logic_error(
        "Geometry registration calls must be performed on the SAME instance of "
        "GeometrySystem used on the first call to "
        "RegisterAsSourceForGeometrySystem()");
  }
  GeometryId id;
  // TODO(amcastro-tri): Consider doing this after finalize so that we can
  // register anchored geometry on ANY body welded to the world.
  if (body.index() == world_index()) {
    id = RegisterAnchoredGeometry(X_BG, shape, geometry_system);
  } else {
    id = RegisterGeometry(body, X_BG, shape, geometry_system);
  }
  const int collision_index = geometry_id_to_collision_index_.size();
  geometry_id_to_collision_index_[id] = collision_index;
}

template<typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterGeometry(
    const Body<T>& body,
    const Isometry3<double>& X_BG, const geometry::Shape& shape,
    geometry::GeometrySystem<T>* geometry_system) {
  DRAKE_ASSERT(!is_finalized());
  DRAKE_ASSERT(geometry_source_is_registered());
  DRAKE_ASSERT(geometry_system == geometry_system_);
  // If not already done, register a frame for this body.
  if (!body_has_registered_frame(body)) {
    body_index_to_frame_id_[body.index()] =
        geometry_system->RegisterFrame(
            source_id_.value(),
            GeometryFrame(
                body.name(),
                /* Initial pose: Not really used by GS. Will get removed. */
                Isometry3<double>::Identity()));
  }

  // Register geometry in the body frame.
  GeometryId geometry_id = geometry_system->RegisterGeometry(
      source_id_.value(), body_index_to_frame_id_[body.index()],
      std::make_unique<GeometryInstance>(X_BG, shape.Clone()));
  geometry_id_to_body_index_[geometry_id] = body.index();
  return geometry_id;
}

template<typename T>
geometry::GeometryId MultibodyPlant<T>::RegisterAnchoredGeometry(
    const Isometry3<double>& X_WG, const geometry::Shape& shape,
    geometry::GeometrySystem<T>* geometry_system) {
  DRAKE_ASSERT(!is_finalized());
  DRAKE_ASSERT(geometry_source_is_registered());
  DRAKE_ASSERT(geometry_system == geometry_system_);
  GeometryId geometry_id = geometry_system->RegisterAnchoredGeometry(
      source_id_.value(),
      std::make_unique<GeometryInstance>(X_WG, shape.Clone()));
  geometry_id_to_body_index_[geometry_id] = world_index();
  return geometry_id;
}

template<typename T>
void MultibodyPlant<T>::Finalize() {
  model_->Finalize();
  FinalizePlantOnly();
}

template<typename T>
void MultibodyPlant<T>::FinalizePlantOnly() {
  DeclareStateAndPorts();
  // Only declare ports to communicate with a GeometrySystem if the plant is
  // provided with a valid source id.
  if (source_id_) DeclareGeometrySystemPorts();
  DeclareCacheEntries();
  geometry_system_ = nullptr;  // must not be used after Finalize().
  if (get_num_collision_geometries() > 0 &&
      penalty_method_contact_parameters_.time_scale < 0)
    set_penetration_allowance();
}

template<typename T>
std::unique_ptr<systems::LeafContext<T>>
MultibodyPlant<T>::DoMakeLeafContext() const {
  DRAKE_THROW_UNLESS(is_finalized());
  return std::make_unique<MultibodyTreeContext<T>>(model_->get_topology());
}

template<typename T>
void MultibodyPlant<T>::DoCalcTimeDerivatives(
    const systems::Context<T>& context,
    systems::ContinuousState<T>* derivatives) const {
  const auto x =
      dynamic_cast<const systems::BasicVector<T>&>(
          context.get_continuous_state_vector()).get_value();
  const int nv = this->num_velocities();

  // Allocate workspace. We might want to cache these to avoid allocations.
  // Mass matrix.
  MatrixX<T> M(nv, nv);
  // Forces.
  MultibodyForces<T> forces(*model_);
  // Bodies' accelerations, ordered by BodyNodeIndex.
  std::vector<SpatialAcceleration<T>> A_WB_array(model_->num_bodies());
  // Generalized accelerations.
  VectorX<T> vdot = VectorX<T>::Zero(nv);

  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  const VelocityKinematicsCache<T>& vc = EvalVelocityKinematics(context);

  // Compute forces applied through force elements. This effectively resets
  // the forces to zero and adds in contributions due to force elements.
  model_->CalcForceElementsContribution(context, pc, vc, &forces);

  // If there is any input actuation, add it to the multibody forces.
  if (num_actuators() > 0) {
    Eigen::VectorBlock<const VectorX<T>> u =
        this->EvalEigenVectorInput(context, actuation_port_);
    for (JointActuatorIndex actuator_index(0);
         actuator_index < num_actuators(); ++actuator_index) {
      const JointActuator<T>& actuator =
          model().get_joint_actuator(actuator_index);
      // We only support actuators on single dof joints for now.
      DRAKE_DEMAND(actuator.joint().num_dofs() == 1);
      for (int joint_dof = 0;
           joint_dof < actuator.joint().num_dofs(); ++joint_dof) {
        actuator.AddInOneForce(context, joint_dof, u[actuator_index], &forces);
      }
    }
  }

  model_->CalcMassMatrixViaInverseDynamics(context, &M);

  // WARNING: to reduce memory foot-print, we use the input applied arrays also
  // as output arrays. This means that both the array of applied body forces and
  // the array of applied generalized forces get overwritten on output. This is
  // not important in this case since we don't need their values anymore.
  // Please see the documentation for CalcInverseDynamics() for details.

  // With vdot = 0, this computes:
  //   tau = C(q, v)v - tau_app - ∑ J_WBᵀ(q) Fapp_Bo_W.
  std::vector<SpatialForce<T>>& F_BBo_W_array = forces.mutable_body_forces();
  VectorX<T>& tau_array = forces.mutable_generalized_forces();

  // Compute contact forces on each body by penalty method.
  if (get_num_collision_geometries() > 0) {
    CalcAndAddContactForcesByPenaltyMethod(context, pc, vc, &F_BBo_W_array);
  }

  model_->CalcInverseDynamics(
      context, pc, vc, vdot,
      F_BBo_W_array, tau_array,
      &A_WB_array,
      &F_BBo_W_array, /* Notice these arrays gets overwritten on output. */
      &tau_array);

  vdot = M.ldlt().solve(-tau_array);

  auto v = x.bottomRows(nv);
  VectorX<T> xdot(this->num_multibody_states());
  VectorX<T> qdot(this->num_positions());
  model_->MapVelocityToQDot(context, v, &qdot);
  xdot << qdot, vdot;
  derivatives->SetFromVector(xdot);
}

template<typename T>
void MultibodyPlant<T>::set_penetration_allowance(
    double penetration_allowance) {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  // Default to Earth's gravity for this estimation.
  const double g = gravity_field_.has_value() ?
                   gravity_field_.value()->gravity_vector().norm() : 9.81;

  // TODO(amcastro-tri): Improve this heuristics in future PR's for when there
  // are several flying objects and fixed base robots (E.g.: manipulation
  // cases.)

  // The heuristic now is very simple. We should update it to:
  //  - Only scan free bodies for weight.
  //  - Consider an estimate of maximum velocities (context dependent).
  // Right now we are being very conservative and use the maximum mass in the
  // system.
  double mass = 0.0;
  for (BodyIndex body_index(0); body_index < num_bodies(); ++body_index) {
    const Body<T>& body = model().get_body(body_index);
    mass = std::max(mass, body.get_default_mass());
  }

  // For now, we use the model of a critically damped spring mass oscillator
  // to estimate these parameters: mẍ+cẋ+kx=mg
  // Notice however that normal forces are computed according to: fₙ=kx(1+dẋ)
  // which translate to a second order oscillator of the form:
  // mẍ+(kdx)ẋ+kx=mg
  // Therefore, for this more complex, non-linear, oscillator, we estimate the
  // damping constant d using a time scale related to the free oscillation
  // (omega below) and the requested penetration allowance as a length scale.

  // We first estimate the stiffness based on static equilibrium.
  const double stiffness = mass * g / penetration_allowance;
  // Frequency associated with the stiffness above.
  const double omega = sqrt(stiffness / mass);

  // Estimated contact time scale. The relative velocity of objects coming into
  // contact goes to zero in this time scale.
  const double time_scale = 1.0 / omega;

  // Damping ratio for a critically damped model. We could allow users to set
  // this. Right now, critically damp the normal direction.
  // This corresponds to a non-penetraion constraint in the limit for
  // contact_penetration_allowance_ goint to zero (no bounce off).
  const double damping_ratio = 1.0;
  // We form the damping (with units of 1/velocity) using dimensional analysis.
  // Thus we use 1/omega for the time scale and penetration_allowance for the
  // length scale. We then scale it by the damping ratio.
  const double damping = damping_ratio * time_scale / penetration_allowance;

  // Final parameters used in the penalty method:
  penalty_method_contact_parameters_.stiffness = stiffness;
  penalty_method_contact_parameters_.damping = damping;
  // The time scale can be requested to hint the integrator's time step.
  penalty_method_contact_parameters_.time_scale = time_scale;
}

template<>
void MultibodyPlant<double>::CalcAndAddContactForcesByPenaltyMethod(
    const systems::Context<double>& context,
    const PositionKinematicsCache<double>& pc,
    const VelocityKinematicsCache<double>& vc,
    std::vector<SpatialForce<double>>* F_BBo_W_array) const {
  if (get_num_collision_geometries() == 0) return;

  const geometry::QueryObject<double>& query_object =
      this->EvalAbstractInput(context, geometry_query_port_)
          ->template GetValue<geometry::QueryObject<double>>();

  std::vector<PenetrationAsPointPair<double>> penetrations =
      query_object.ComputePointPairPenetration();
  for (const auto& penetration : penetrations) {
    const GeometryId geometryA_id = penetration.id_A;
    const GeometryId geometryB_id = penetration.id_B;

    // TODO(amcastro-tri): Request GeometrySystem to do this filtering for us
    // when that capability lands.
    // TODO(amcastro-tri): consider allowing this id's to belong to a third
    // external system when they correspond to anchored geometry.
    if (!is_collision_geometry(geometryA_id) ||
        !is_collision_geometry(geometryB_id))
      continue;

    BodyIndex bodyA_index = geometry_id_to_body_index_.at(penetration.id_A);
    BodyIndex bodyB_index = geometry_id_to_body_index_.at(penetration.id_B);

    BodyNodeIndex bodyA_node_index =
        model().get_body(bodyA_index).node_index();
    BodyNodeIndex bodyB_node_index =
        model().get_body(bodyB_index).node_index();

    // Penetration depth, > 0 during penetration.
    const double &x = penetration.depth;
    const Vector3<double>& nhat_BA_W = penetration.nhat_BA_W;
    const Vector3<double>& p_WCa = penetration.p_WCa;
    const Vector3<double>& p_WCb = penetration.p_WCb;

    // Contact point C.
    const Vector3<double> p_WC = 0.5 * (p_WCa + p_WCb);

    // Contact point position on body A.
    const Vector3<double>& p_WAo =
        pc.get_X_WB(bodyA_node_index).translation();
    const Vector3<double>& p_CoAo_W = p_WAo - p_WC;

    // Contact point position on body B.
    const Vector3<double>& p_WBo =
        pc.get_X_WB(bodyB_node_index).translation();
    const Vector3<double>& p_CoBo_W = p_WBo - p_WC;

    // Separation velocity, > 0  if objects separate.
    const Vector3<double> v_WAc =
        vc.get_V_WB(bodyA_node_index).Shift(-p_CoAo_W).translational();
    const Vector3<double> v_WBc =
        vc.get_V_WB(bodyB_node_index).Shift(-p_CoBo_W).translational();
    const Vector3<double> v_AcBc_W = v_WBc - v_WAc;

    // if xdot = vn > 0 ==> they are getting closer.
    const double vn = v_AcBc_W.dot(nhat_BA_W);

    // Magnitude of the normal force on body A at contact point C.
    const double k = penalty_method_contact_parameters_.stiffness;
    const double d = penalty_method_contact_parameters_.damping;
    const double fn_AC = k * x * (1.0 + d * vn);

    if (fn_AC <= 0) continue;  // Continue with next point.

    // Spatial force on body A at C, expressed in the world frame W.
    const SpatialForce<double> F_AC_W(Vector3<double>::Zero(),
                                      fn_AC * nhat_BA_W);

    if (bodyA_index != world_index()) {
      // Spatial force on body A at Ao, expressed in W.
      const SpatialForce<double> F_AAo_W = F_AC_W.Shift(p_CoAo_W);
      F_BBo_W_array->at(bodyA_index) += F_AAo_W;
    }

    if (bodyB_index != world_index()) {
      // Spatial force on body B at Bo, expressed in W.
      const SpatialForce<double> F_BBo_W = -F_AC_W.Shift(p_CoBo_W);
      F_BBo_W_array->at(bodyB_index) += F_BBo_W;
    }
  }
}

template<typename T>
void MultibodyPlant<T>::CalcAndAddContactForcesByPenaltyMethod(
    const systems::Context<T>&,
    const PositionKinematicsCache<T>&, const VelocityKinematicsCache<T>&,
    std::vector<SpatialForce<T>>*) const {
  DRAKE_ABORT_MSG("Only <double> is supported.");
}

template<typename T>
void MultibodyPlant<T>::DoMapQDotToVelocity(
    const systems::Context<T>& context,
    const Eigen::Ref<const VectorX<T>>& qdot,
    systems::VectorBase<T>* generalized_velocity) const {
  const int nq = model_->num_positions();
  const int nv = model_->num_velocities();

  DRAKE_ASSERT(qdot.size() == nq);
  DRAKE_DEMAND(generalized_velocity != nullptr);
  DRAKE_DEMAND(generalized_velocity->size() == nv);

  VectorX<T> v(nv);
  model_->MapQDotToVelocity(context, qdot, &v);
  generalized_velocity->SetFromVector(v);
}

template<typename T>
void MultibodyPlant<T>::DoMapVelocityToQDot(
    const systems::Context<T>& context,
    const Eigen::Ref<const VectorX<T>>& generalized_velocity,
    systems::VectorBase<T>* positions_derivative) const {
  const int nq = model_->num_positions();
  const int nv = model_->num_velocities();

  DRAKE_ASSERT(generalized_velocity.size() == nv);
  DRAKE_DEMAND(positions_derivative != nullptr);
  DRAKE_DEMAND(positions_derivative->size() == nq);

  VectorX<T> qdot(nq);
  model_->MapVelocityToQDot(context, generalized_velocity, &qdot);
  positions_derivative->SetFromVector(qdot);
}

template<typename T>
void MultibodyPlant<T>::DeclareStateAndPorts() {
  // The model must be finalized.
  DRAKE_DEMAND(this->is_finalized());

  this->DeclareContinuousState(
      BasicVector<T>(model_->num_states()),
      model_->num_positions(),
      model_->num_velocities(), 0 /* num_z */);

  if (num_actuators() > 0) {
    actuation_port_ =
        this->DeclareVectorInputPort(
            systems::BasicVector<T>(num_actuated_dofs())).get_index();
  }

  continuous_state_output_port_ =
      this->DeclareVectorOutputPort(
          BasicVector<T>(num_multibody_states()),
          &MultibodyPlant::CopyContinuousStateOut).get_index();
}

template <typename T>
void MultibodyPlant<T>::CopyContinuousStateOut(
    const Context<T>& context, BasicVector<T>* state_vector) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  state_vector->SetFrom(context.get_continuous_state_vector());
}

template <typename T>
const systems::InputPortDescriptor<T>&
MultibodyPlant<T>::get_actuation_input_port() const {
  DRAKE_THROW_UNLESS(is_finalized());
  DRAKE_THROW_UNLESS(num_actuators() > 0);
  return systems::System<T>::get_input_port(actuation_port_);
}

template <typename T>
const systems::OutputPort<T>&
MultibodyPlant<T>::get_continuous_state_output_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  return this->get_output_port(continuous_state_output_port_);
}

template<typename T>
void MultibodyPlant<T>::DeclareGeometrySystemPorts() {
  geometry_query_port_ = this->DeclareAbstractInputPort().get_index();
  geometry_id_port_ =
      this->DeclareAbstractOutputPort(
          &MultibodyPlant::AllocateFrameIdOutput,
          &MultibodyPlant::CalcFrameIdOutput).get_index();
  geometry_pose_port_ =
      this->DeclareAbstractOutputPort(
          &MultibodyPlant::AllocateFramePoseOutput,
          &MultibodyPlant::CalcFramePoseOutput).get_index();
  // Compute once, and for all, a vector of ids to be used in CalcFrameIdOuput.
  // ids_ does not change after it is created here.
  // Note: the important bit here is that both, CalcFrameIdOutput() and
  // CalcFramePoseOutput(), scan body_index_to_frame_id_ in the same order so
  // that the ids port is consistent with the poses port.
  for (auto it : body_index_to_frame_id_) {
    ids_.push_back(it.second);
  }
}

template <typename T>
FrameIdVector MultibodyPlant<T>::AllocateFrameIdOutput(
    const Context<T>&) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(source_id_ != nullopt);
  // User must be done adding elements to the model.
  DRAKE_DEMAND(model_->topology_is_valid());
  return FrameIdVector(source_id_.value(), ids_);
}

template <typename T>
void MultibodyPlant<T>::CalcFrameIdOutput(
    const Context<T>&, FrameIdVector* ids_vector) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  // Just a sanity check.
  DRAKE_DEMAND(source_id_ != nullopt);
  *ids_vector = FrameIdVector(source_id_.value(), ids_);
}

template <typename T>
FramePoseVector<T> MultibodyPlant<T>::AllocateFramePoseOutput(
    const Context<T>&) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(source_id_ != nullopt);
  FramePoseVector<T> poses(source_id_.value());
  // Only the pose for bodies for which geometry has been registered needs to
  // be placed in the output.
  const int num_bodies_with_geometry = body_index_to_frame_id_.size();
  poses.mutable_vector().resize(num_bodies_with_geometry);
  return poses;
}

template <typename T>
void MultibodyPlant<T>::CalcFramePoseOutput(
    const Context<T>& context, FramePoseVector<T>* poses) const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);

  FramePoseVector<T> new_poses(get_source_id().value());
  std::vector<Isometry3<T>>& pose_data = new_poses.mutable_vector();
  pose_data.resize(body_index_to_frame_id_.size());
  // TODO(amcastro-tri): Make use of Body::EvalPoseInWorld(context) once caching
  // lands.
  int pose_index = 0;
  for (const auto it : body_index_to_frame_id_) {
    const BodyIndex body_index = it.first;
    const Body<T>& body = model_->get_body(body_index);
    pose_data[pose_index++] = pc.get_X_WB(body.node_index());
  }
  *poses = new_poses;
}

template <typename T>
const OutputPort<T>& MultibodyPlant<T>::get_geometry_ids_output_port()
const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(geometry_source_is_registered());
  return systems::System<T>::get_output_port(geometry_id_port_);
}

template <typename T>
const OutputPort<T>& MultibodyPlant<T>::get_geometry_poses_output_port()
const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(geometry_source_is_registered());
  return systems::System<T>::get_output_port(geometry_pose_port_);
}

template <typename T>
const systems::InputPortDescriptor<T>&
MultibodyPlant<T>::get_geometry_query_input_port() const {
  DRAKE_MBP_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(geometry_source_is_registered());
  return systems::System<T>::get_input_port(geometry_query_port_);
}

template<typename T>
void MultibodyPlant<T>::DeclareCacheEntries() {
  // TODO(amcastro-tri): User proper System::Declare() infrastructure to
  // declare cache entries when that lands.
  pc_ = std::make_unique<PositionKinematicsCache<T>>(model_->get_topology());
  vc_ = std::make_unique<VelocityKinematicsCache<T>>(model_->get_topology());
}

template<typename T>
const PositionKinematicsCache<T>& MultibodyPlant<T>::EvalPositionKinematics(
    const systems::Context<T>& context) const {
  // TODO(amcastro-tri): Replace Calc() for an actual Eval() when caching lands.
  model_->CalcPositionKinematicsCache(context, pc_.get());
  return *pc_;
}

template<typename T>
const VelocityKinematicsCache<T>& MultibodyPlant<T>::EvalVelocityKinematics(
    const systems::Context<T>& context) const {
  // TODO(amcastro-tri): Replace Calc() for an actual Eval() when caching lands.
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  model_->CalcVelocityKinematicsCache(context, pc, vc_.get());
  return *vc_;
}

template <typename T>
void MultibodyPlant<T>::ThrowIfFinalized(const char* source_method) const {
  if (is_finalized()) {
    throw std::logic_error(
        "Post-finalize calls to '" + std::string(source_method) + "()' are "
        "not allowed; calls to this method must happen before Finalize().");
  }
}

template <typename T>
void MultibodyPlant<T>::ThrowIfNotFinalized(const char* source_method) const {
  if (!is_finalized()) {
    throw std::logic_error(
        "Pre-finalize calls to '" + std::string(source_method) + "()' are "
        "not allowed; you must call Finalize() first.");
  }
}

}  // namespace multibody_plant
}  // namespace multibody
}  // namespace drake

DRAKE_DEFINE_CLASS_TEMPLATE_INSTANTIATIONS_ON_DEFAULT_NONSYMBOLIC_SCALARS(
    class drake::multibody::multibody_plant::MultibodyPlant)
