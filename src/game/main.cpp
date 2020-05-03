
#include <rynx/application/application.hpp>
#include <rynx/application/visualisation/debug_visualisation.hpp>
#include <rynx/application/logic.hpp>
#include <rynx/application/render.hpp>
#include <rynx/application/simulation.hpp>
#include <rynx/application/render.hpp>

#include <rynx/menu/Button.hpp>
#include <rynx/menu/Slider.hpp>
#include <rynx/menu/Div.hpp>

#include <rynx/graphics/framebuffer.hpp>
#include <rynx/graphics/renderer/screenspace.hpp>
#include <rynx/graphics/renderer/meshrenderer.hpp>
#include <rynx/graphics/mesh/shape.hpp>
#include <rynx/math/geometry/polygon_triangulation.hpp>
#include <rynx/graphics/text/fontdata/lenka.hpp>
#include <rynx/graphics/text/fontdata/consolamono.hpp>

#include <rynx/rulesets/frustum_culling.hpp>
#include <rynx/rulesets/motion.hpp>
#include <rynx/rulesets/physics/springs.hpp>
#include <rynx/rulesets/collisions.hpp>
#include <rynx/rulesets/particles.hpp>

#include <rynx/tech/smooth_value.hpp>
#include <rynx/tech/timer.hpp>
#include <rynx/tech/ecs.hpp>

#include <rynx/tech/components.hpp>
#include <rynx/application/components.hpp>

#include <rynx/input/mapped_input.hpp>
#include <rynx/scheduler/task_scheduler.hpp>

#include <iostream>
#include <thread>

#include <cmath>

#include <rynx/audio/audio.hpp>

template<typename T>
struct range {
	range(T b, T e) : begin(b), end(e) {}
	range() = default;

	T begin;
	T end;

	T operator()(float v) const {
		return static_cast<T>(begin * (1.0f - v) + end * v);
	}
};

namespace rynx {
	namespace components {
		struct position_relative {
			rynx::ecs::id host;
			vec3f relative_pos;
		};
	}
}

int main(int argc, char** argv) {

	// uses this thread services of rynx, for example in cpu performance profiling.
	rynx::this_thread::rynx_thread_raii rynx_thread_services_required_token;

	Font fontLenka(Fonts::setFontLenka());
	Font fontConsola(Fonts::setFontConsolaMono());

	rynx::application::Application application;
	application.openWindow(1920, 1080);
	application.loadTextures("../textures/textures.txt");
	application.meshRenderer().loadDefaultMesh("Empty");

	auto meshes = application.meshRenderer().meshes();
	{
		meshes->create("ball", rynx::Shape::makeCircle(1.0f, 32), "Hero");
		meshes->create("circle_empty", rynx::Shape::makeCircle(1.0f, 32), "Empty");
		meshes->create("square_empty", rynx::Shape::makeBox(1.0f), "Empty");
		meshes->create("particle_smoke", rynx::Shape::makeBox(1.0f), "Smoke");
		meshes->create("square_rope", rynx::Shape::makeBox(1.0f), "Rope");

		auto* tube_mesh = meshes->create("square_tube_normals", rynx::Shape::makeBox(1.0f), "Empty");
		tube_mesh->normals.clear();
		tube_mesh->putNormal(0, +1, 0);
		tube_mesh->putNormal(0, -1, 0);
		tube_mesh->putNormal(0, -1, 0);
		tube_mesh->putNormal(0, +1, 0);
		tube_mesh->bind();
		tube_mesh->rebuildNormalBuffer();
	}

	rynx::scheduler::task_scheduler scheduler;
	rynx::application::simulation base_simulation(scheduler);
	rynx::ecs& ecs = base_simulation.m_ecs;

	std::shared_ptr<rynx::camera> camera = std::make_shared<rynx::camera>();
	camera->setProjection(0.02f, 20000.0f, application.aspectRatio());

	rynx::mapped_input gameInput(application.input());


	rynx::collision_detection* detection = new rynx::collision_detection();
	auto& collisionDetection = *detection;

	// setup collision detection
	rynx::collision_detection::category_id collisionCategoryDynamic = collisionDetection.add_category();
	rynx::collision_detection::category_id collisionCategoryStatic = collisionDetection.add_category();
	rynx::collision_detection::category_id collisionCategoryProjectiles = collisionDetection.add_category();

	{
		collisionDetection.enable_collisions_between(collisionCategoryDynamic, collisionCategoryDynamic); // enable dynamic <-> dynamic collisions
		collisionDetection.enable_collisions_between(collisionCategoryDynamic, collisionCategoryStatic.ignore_collisions()); // enable dynamic <-> static collisions

		collisionDetection.enable_collisions_between(collisionCategoryProjectiles, collisionCategoryStatic.ignore_collisions()); // projectile <-> static
		collisionDetection.enable_collisions_between(collisionCategoryProjectiles, collisionCategoryDynamic); // projectile <-> dynamic
	}

	class sound_mapper {
	public:
		void insert(std::string name, int value) {
			m_data[name].emplace_back(value);
		}

		int get(const std::string& name) {
			auto it = m_data.find(name);
			if (it != m_data.end()) {
				if (!it->second.empty()) {
					return it->second[m_random(it->second.size())];
				}
			}
			return 0;
		}

	private:
		rynx::math::rand64 m_random;
		rynx::unordered_map<std::string, std::vector<int>> m_data;
	};

	rynx::sound::audio_system audio;
	sound_mapper sounds;

	// set additional resources your simulation wants to use.
	{
		base_simulation.set_resource(&collisionDetection);
		base_simulation.set_resource(&gameInput);
		base_simulation.set_resource(&audio);
		base_simulation.set_resource(&sounds);
	}

	struct player_controlled { int controller_index = 0; };
	struct health {
		float max = 100;
		float current = 100;
	};
	struct ship_engine_state {
		rynx::ecs::id id;
		float activity = 0;
		float phase = 0;
		bool is_roaring = false;
	};

	sounds.insert("engine", audio.load("../sound/bass/engine01.ogg"));
	sounds.insert("engine", audio.load("../sound/bass/engine02.ogg"));
	sounds.insert("engine", audio.load("../sound/bass/engine03.ogg"));
	sounds.insert("engine", audio.load("../sound/bass/engine04.ogg"));
	sounds.insert("engine", audio.load("../sound/bass/engine05.ogg"));

	sounds.insert("steering", audio.load("../sound/ship/gas_leak01.ogg"));
	sounds.insert("steering", audio.load("../sound/ship/gas_leak02.ogg"));
	sounds.insert("steering", audio.load("../sound/ship/gas_leak03.ogg"));
	sounds.insert("steering", audio.load("../sound/ship/gas_leak04.ogg"));
	sounds.insert("steering", audio.load("../sound/ship/gas_leak05.ogg"));

	sounds.insert("engine_ignition_boom", audio.load("../sound/engine_boom.ogg"));

	class player_controls : public rynx::application::logic::iruleset {
		int32_t action_turn_left[2] = { 0 };
		int32_t action_turn_right[2] = { 0 };
		int32_t action_accelerate[2] = { 0 };
		int32_t action_decelerate[2] = { 0 };

		rynx::math::rand64 random;

		rynx::sound::configuration engine_conf;
		rynx::sound::configuration steering_conf;

	public:
		player_controls(rynx::mapped_input& input) {
			action_turn_left[0] = input.generateAndBindGameKey('A', "player_turn_left");
			action_turn_right[0] = input.generateAndBindGameKey('D', "player_turn_right");
			action_accelerate[0] = input.generateAndBindGameKey('W', "player_accelerate");
			action_decelerate[0] = input.generateAndBindGameKey('S', "player_decelerate");

			action_turn_left[1] = input.generateAndBindGameKey('A', "player_turn_left");
			action_turn_right[1] = input.generateAndBindGameKey('D', "player_turn_right");
			action_accelerate[1] = input.generateAndBindGameKey('W', "player_accelerate");
			action_decelerate[1] = input.generateAndBindGameKey('S', "player_decelerate");
		}
		
		virtual ~player_controls() {}
		
		virtual void onFrameProcess(rynx::scheduler::context& context, float dt) override {
			context.add_task("player input", [this, dt](
				rynx::scheduler::task& context,
				const rynx::mapped_input& input,
				rynx::sound::audio_system& sound,
				sound_mapper& sound_map,
				rynx::ecs::view<
					const player_controlled,
					const rynx::components::position,
					rynx::components::motion,
					ship_engine_state,
					rynx::components::light_omni> ecs)
			{
				struct engine_fumes {
					range<rynx::vec3f> direction;
					range<rynx::vec3f> position;
					range<float> radius;
					range<rynx::floats4> color;
					range<float> lifetime;
					range<int> number;
				};
				
				std::vector<engine_fumes> fumes;
				ecs.query().for_each([&](player_controlled player, rynx::components::motion& motion, rynx::components::position position, ship_engine_state& engine) {
					float turn_amount = 0;
					float accelerate_amount = 0;
					turn_amount += 220.00f * input.isKeyDown(action_turn_left[player.controller_index]);
					turn_amount -= 220.00f * input.isKeyDown(action_turn_right[player.controller_index]);
					accelerate_amount += 1000.00f * input.isKeyDown(action_accelerate[player.controller_index]) * (engine.activity > 0.95f);
					accelerate_amount -= 500.00f * input.isKeyDown(action_decelerate[player.controller_index]);

					rynx::vec3f forward(std::cos(position.angle), std::sin(position.angle), 0);

					motion.angularAcceleration += turn_amount;
					motion.acceleration += forward * accelerate_amount;

					engine_fumes f;
					f.radius = { 0.5f, 1.2f };
					f.color = { rynx::floats4(0.7f, 0.7f, 0.1f, 0.7f), rynx::floats4(1.0f, 1.0f, 0.5f, 0.9f) };
					f.number = { 1, 5 };
					f.lifetime = { 0.1f, 0.2f };

					if (input.isKeyDown(action_turn_left[player.controller_index])) {
						f.position = { position.value + forward * 2.8f, position.value + forward * 3.0f };
						f.direction = { motion.velocity * dt + rynx::math::rotatedXY(forward, -rynx::math::pi * 0.5f * 0.8f), motion.velocity * dt + rynx::math::rotatedXY(forward, -rynx::math::pi * 0.5f * 1.2f) };
						fumes.emplace_back(f);

						f.position = { position.value - forward * 2.8f, position.value - forward * 3.0f };
						f.direction = { motion.velocity * dt + rynx::math::rotatedXY(forward, +rynx::math::pi * 0.5f * 0.8f), motion.velocity * dt + rynx::math::rotatedXY(forward, +rynx::math::pi * 0.5f * 1.2f) };
						fumes.emplace_back(f);

						if (steering_conf.completion_rate() > 0.5f) {
							steering_conf = sound.play_sound(sound_map.get("steering"), rynx::vec3f(), rynx::vec3f());
							steering_conf.set_loudness(0.1f);
							steering_conf.set_pitch_shift(-0.45f);
						}
					}

					if (input.isKeyDown(action_turn_right[player.controller_index])) {
						f.position = { position.value + forward * 2.8f, position.value + forward * 3.0f };
						f.direction = { motion.velocity * dt + rynx::math::rotatedXY(forward, +rynx::math::pi * 0.5f * 0.8f), motion.velocity * dt + rynx::math::rotatedXY(forward, +rynx::math::pi * 0.5f * 1.2f) };
						fumes.emplace_back(f);

						f.position = { position.value - forward * 2.8f, position.value - forward * 3.0f };
						f.direction = { motion.velocity * dt + rynx::math::rotatedXY(forward, -rynx::math::pi * 0.5f * 0.8f), motion.velocity * dt + rynx::math::rotatedXY(forward, -rynx::math::pi * 0.5f * 1.2f) };
						fumes.emplace_back(f);

						if (steering_conf.completion_rate() > 0.5f) {
							steering_conf = sound.play_sound(sound_map.get("steering"), rynx::vec3f(), rynx::vec3f());
							steering_conf.set_loudness(0.1f);
							steering_conf.set_pitch_shift(-0.35f);
						}
					}

					if (input.isKeyDown(action_decelerate[player.controller_index])) {
						f.position = { position.value + forward * 2.8f, position.value + forward * 3.0f };
						f.direction = { motion.velocity * dt + rynx::math::rotatedXY(forward, -0.7f), motion.velocity * dt + rynx::math::rotatedXY(forward, -0.3f) };
						f.lifetime = { 0.4f, 0.7f };
						fumes.emplace_back(f);

						f.direction = { rynx::math::rotatedXY(forward, +0.7f), rynx::math::rotatedXY(forward, +0.3f) };
						fumes.emplace_back(f);

						if (steering_conf.completion_rate() > 0.5f) {
							steering_conf = sound.play_sound(sound_map.get("steering"), rynx::vec3f(), rynx::vec3f());
							steering_conf.set_loudness(0.15f);
							steering_conf.set_pitch_shift(-0.5f);
						}
					}

					bool engine_is_active = engine.activity > 0.95f;
					float main_engine_max_per_sound = 0.3f;
					float engine_sound_loudness_old = engine.activity < 1.0f ? engine.activity * engine.activity * engine.activity * engine.activity * engine.activity * main_engine_max_per_sound : main_engine_max_per_sound;
					engine_conf.set_loudness(engine_sound_loudness_old);
					if (engine.is_roaring) {
						engine_conf.set_pitch_shift(0.4f * std::sin(engine.phase));
					}

					if (input.isKeyDown(action_accelerate[player.controller_index])) {
						engine.activity += (1.0f - engine.activity) * dt * 5;
						bool mega_boom = !engine_is_active && engine.activity > 0.95f && !engine.is_roaring;
						if (mega_boom) {
							engine.is_roaring = true;
							auto conf = sound.play_sound(sound_map.get("engine_ignition_boom"), rynx::vec3f(), rynx::vec3f(), 0.5f);
							engine.activity = 3.5f;
							conf.set_pitch_shift(-0.25f);
						}

						if (engine.is_roaring) {
							f.position = { position.value - forward * 2.0f, position.value + forward * 3.0f };
							f.direction = { rynx::math::rotatedXY(-forward, +0.6f), rynx::math::rotatedXY(-forward, -0.6f) };
							f.number = { 3, 7 };
							if (mega_boom)
								f.number = { 300 , 700 };

							f.radius = { 1.0f, 2.0f };
							f.lifetime = { 0.2f, 0.5f };
							fumes.emplace_back(f);
						}

						if (engine_conf.completion_rate() > 0.66f) {
							if (engine.is_roaring) {
								engine_conf = sound.play_sound(sound_map.get("engine"), rynx::vec3f(), rynx::vec3f());
								engine_conf.set_loudness(engine.activity < 1.0f ? engine.activity * engine.activity * engine.activity * engine.activity * engine.activity * main_engine_max_per_sound : main_engine_max_per_sound);
								engine_conf.set_pitch_shift(0.1f * std::sin(engine.phase));
							}
							else {
								engine_conf = sound.play_sound(sound_map.get("engine"), rynx::vec3f(), rynx::vec3f());
								engine_conf.set_loudness(engine_sound_loudness_old);
								engine_conf.set_pitch_shift(1.0f - 0.5f * std::min(engine.activity, 1.0f));
							}
						}
					}
					else {
						engine.activity += (0.0f - engine.activity) * dt * 2;

						if (engine_conf.completion_rate() > 0.66f && engine.activity > 0.1f) {
							if (engine.is_roaring) {
								engine_conf = sound.play_sound(sound_map.get("engine"), rynx::vec3f(), rynx::vec3f());
								engine_conf.set_loudness(engine_sound_loudness_old);
								engine_conf.set_pitch_shift(0.4f * std::sin(engine.phase));
							}
							else {
								engine_conf = sound.play_sound(sound_map.get("engine"), rynx::vec3f(), rynx::vec3f());
								engine_conf.set_loudness(engine_sound_loudness_old);
								engine_conf.set_pitch_shift(1.0f - 0.5f * std::min(engine.activity, 1.0f));
							}
						}
					}

					engine.phase += engine.activity * 0.01f;
					if (engine.phase > 2 * rynx::math::pi) {
						engine.phase -= 2 * rynx::math::pi;
					}

					auto& engine_light = ecs[engine.id].get<rynx::components::light_omni>();
					engine_light.color.a = 20.0f * engine.activity * engine.activity;
					engine_light.ambient = std::clamp(engine.activity * engine.activity, 0.0f, 1.0f);
					if (engine.activity < 0.25f)
						engine.is_roaring = false;
				});

				if (!fumes.empty()) {
					context.make_task("create engine fumes", [this, fumes = std::move(fumes)](rynx::ecs& ecs) {
						std::vector<rynx::vec3f> particle_positions;
						for (auto&& fume : fumes) {
							int num_fumes = fume.number(random());
							for (int i = 0; i < num_fumes; ++i) {
								rynx::components::particle_info p_info;
								p_info.color.begin = fume.color(random());
								p_info.color.end = p_info.color.begin;
								p_info.color.end.a = 0.0f;
								p_info.color.end.r *= 0.5f;
								p_info.color.end.g *= 0.5f;
								p_info.radius.begin = fume.radius(random());
								p_info.radius.end = p_info.radius.begin * 2.0f;

								float quadratic_favor_middle = random(-1.0f, +1.0f) * random(-1.0f, +1.0f);
								float lifetime_modifier = 1.0f - std::abs(quadratic_favor_middle);
								lifetime_modifier *= lifetime_modifier;

								quadratic_favor_middle = quadratic_favor_middle * 0.5f + 0.5f;
								rynx::vec3f fum_pos = fume.position(random());

								auto particle_id = ecs.create(
									p_info,
									rynx::components::position(fum_pos),
									rynx::components::radius(p_info.radius.begin),
									rynx::components::motion(fume.direction(quadratic_favor_middle) * 120 * random(0.6f, 1.8f) * lifetime_modifier, random(-1.0f, +1.0f)),
									rynx::components::lifetime(fume.lifetime(random()) * lifetime_modifier),
									rynx::components::color(p_info.color.begin),
									rynx::components::dampening{ 2.0f, 1.0f },
									rynx::components::translucent()
								);

								particle_positions.emplace_back(ecs[particle_id].get<rynx::components::position>().value);
								rynx_assert((fum_pos - particle_positions.back()).length_squared() < 0.1f, "wtf");
							}

							for (size_t i = 0; i < particle_positions.size(); ++i) {
								for (size_t k = i + 1; k < particle_positions.size(); ++k) {
									if ((particle_positions[i] - particle_positions[k]).length_squared() > 10 * 10) {
										rynx_assert(false, "wtf");
									}
								}
							}
						}
					});
				}
			});
		}
	};
	

	// setup game logic
	{
		// Todo: if we created the rulesets through base simulation, and returned proxy objects that on destructor move themselves into the simulation rules..
		//       we could remove like 50% of this setup code?
		auto ruleset_collisionDetection = std::make_unique<rynx::ruleset::physics_2d>();
		auto ruleset_particle_update = std::make_unique<rynx::ruleset::particle_system>();
		auto ruleset_frustum_culling = std::make_unique<rynx::ruleset::frustum_culling>(camera);

		auto ruleset_motion_updates = std::make_unique<rynx::ruleset::motion_updates>(rynx::vec3<float>(0, -60.8f, 0));
		auto ruleset_physical_springs = std::make_unique<rynx::ruleset::physics::springs>();
		auto ruleset_player_controls = std::make_unique<player_controls>(gameInput);

		ruleset_physical_springs->depends_on(*ruleset_motion_updates);
		ruleset_collisionDetection->depends_on(*ruleset_motion_updates);
		ruleset_frustum_culling->depends_on(*ruleset_motion_updates);
		ruleset_player_controls->depends_on(*ruleset_motion_updates);
		ruleset_player_controls->required_for(*ruleset_collisionDetection);

		base_simulation.add_rule_set(std::move(ruleset_motion_updates));
		base_simulation.add_rule_set(std::move(ruleset_physical_springs));

		base_simulation.add_rule_set(std::move(ruleset_collisionDetection));
		base_simulation.add_rule_set(std::move(ruleset_particle_update));
		base_simulation.add_rule_set(std::move(ruleset_frustum_culling));
		base_simulation.add_rule_set(std::move(ruleset_player_controls));
	}

	rynx::smooth<rynx::vec3<float>> cameraPosition(0.0f, 0.0f, 300.0f);

	rynx::math::rand64 random;
	
	// setup simulation initial state
	{
		std::vector<rynx::ecs::entity_id_t> ship_entities;
		auto ship_id = ecs.create();
		ecs.attachToEntity(ship_id,
			player_controlled(),
			health(),
			rynx::components::position(),
			rynx::components::motion(),
			rynx::components::physical_body(100, 100 * 5, 0.3f, 1.0f, ship_id),
			rynx::components::radius(3.0f),
			rynx::components::collisions{ collisionCategoryDynamic.value },
			rynx::components::color(),
			rynx::components::mesh{ meshes->get("ball") },
			rynx::matrix4(),
			rynx::components::dampening({ 0.56f, 0.01f }),
			rynx::components::frame_collisions()
		);
		
		auto top_part = ecs.create(
			health(),
			rynx::components::position({10, 0, 0}),
			rynx::components::motion(),
			rynx::components::physical_body(100, 100 * 5, 0.3f, 1.0f, ship_id),
			rynx::components::radius(3.0f),
			rynx::components::collisions{ collisionCategoryDynamic.value },
			rynx::components::color(),
			rynx::components::mesh{ meshes->get("ball") },
			rynx::matrix4(),
			rynx::components::dampening({ 0.56f, 0.01f }),
			rynx::components::frame_collisions()
		);

		auto top_part2 = ecs.create(
			health(),
			rynx::components::position({ 20, 0, 0 }),
			rynx::components::motion(),
			rynx::components::physical_body(100, 100 * 5, 0.3f, 1.0f, ship_id),
			rynx::components::radius(3.0f),
			rynx::components::collisions{ collisionCategoryDynamic.value },
			rynx::components::color(),
			rynx::components::mesh{ meshes->get("ball") },
			rynx::matrix4(),
			rynx::components::dampening({ 0.56f, 0.01f }),
			rynx::components::frame_collisions()
		);

		auto landing_fin_left = ecs.create(
			health(),
			rynx::components::position({ -7, +7, 0 }),
			rynx::components::motion(),
			rynx::components::physical_body(100, 100 * 5, 0.3f, 1.0f, ship_id),
			rynx::components::radius(3.0f),
			rynx::components::collisions{ collisionCategoryDynamic.value },
			rynx::components::color(),
			rynx::components::mesh{ meshes->get("ball") },
			rynx::matrix4(),
			rynx::components::dampening({ 0.56f, 0.01f }),
			rynx::components::frame_collisions()
		);

		auto landing_fin_right = ecs.create(
			health(),
			rynx::components::position({ -7, -7, 0 }),
			rynx::components::motion(),
			rynx::components::physical_body(100, 100 * 5, 0.3f, 1.0f, ship_id),
			rynx::components::radius(3.0f),
			rynx::components::collisions{ collisionCategoryDynamic.value },
			rynx::components::color(),
			rynx::components::mesh{ meshes->get("ball") },
			rynx::matrix4(),
			rynx::components::dampening({ 0.56f, 0.01f }),
			rynx::components::frame_collisions()
		);

		auto foo = [&](rynx::ecs::entity_id_t a, rynx::ecs::entity_id_t b, float angle, float length, float offset)
		{
			rynx::components::phys::joint ship_to_top;
			ship_to_top.connect_with_rod().rotation_free();
			ship_to_top.length = length;
			ship_to_top.strength = 25.0f;
			ship_to_top.id_a = a;
			ship_to_top.id_b = b;
			ship_to_top.point_a = { 0, offset, 0 };
			ship_to_top.point_b = { 0, offset, 0 };
			rynx::math::rotateXY(ship_to_top.point_a, angle);
			rynx::math::rotateXY(ship_to_top.point_b, angle);

			ecs.create(ship_to_top);

			if (offset > 0.001f) {
				ship_to_top.point_a *= -1;
				ship_to_top.point_b *= -1;
				ecs.create(ship_to_top);
			}

			if (offset > 1.0f) {
				ship_to_top.length = rynx::math::sqrt_approx(length * length + 4 * offset * offset);
				ship_to_top.point_a *= -1;
				ecs.create(ship_to_top);

				ship_to_top.point_a *= -1;
				ship_to_top.point_b *= -1;
				ecs.create(ship_to_top);
			}
		};

		foo(ship_id, top_part, 0, 10, 5);
		foo(top_part, top_part2, 0, 10, 5);
		foo(ship_id, top_part2, 0, 20, 0);

		foo(ship_id, landing_fin_left, -rynx::math::pi * 0.25f, rynx::math::sqrt_approx(7 * 7 + 7 * 7), 3);
		foo(ship_id, landing_fin_right, +rynx::math::pi * 0.25f, rynx::math::sqrt_approx(7 * 7 + 7 * 7), 3);
		foo(landing_fin_left, landing_fin_right, 0, 14, 0);

		ship_entities.emplace_back(ship_id);
		ship_entities.emplace_back(top_part);
		ship_entities.emplace_back(top_part2);
		ship_entities.emplace_back(landing_fin_left);
		ship_entities.emplace_back(landing_fin_right);

		auto rotate_around = [&](rynx::ecs::entity_id_t id, float angle) {
			auto origin = ecs[id].get<rynx::components::position>();
			for (auto ship_id : ship_entities) {
				auto pos = ecs[ship_id].get<rynx::components::position>();
				auto diff = pos.value - origin.value;
				rynx::math::rotateXY(diff, angle);
				ecs[ship_id].get<rynx::components::position>().value = origin.value + diff;
				ecs[ship_id].get<rynx::components::position>().angle += angle;
			}
		};

		auto translate = [&](rynx::vec3f delta) {
			for (auto ship_id : ship_entities) {
				ecs[ship_id].get<rynx::components::position>().value += delta;
			}
		};

		rotate_around(ship_id, rynx::math::pi * 0.5f); // turn rocket upright at start.
		translate({140, 60, 0});

		auto ship_main_engine = ecs.create(
			rynx::components::position(),
			rynx::components::position_relative{ ship_id, rynx::vec3f(-5.0f, 0, 0) },
			rynx::components::light_omni({ rynx::floats4(1.0f, 1.0f, 1.0f, 20.0f), 0.0f })
		);

		ecs.attachToEntity(ship_id, ship_engine_state{ ship_main_engine, 0, 0.0f, false});

		auto makeBox_inside = [&](rynx::vec3<float> pos, float angle, float edgeLength, float angular_velocity) {
			auto mesh_name = std::to_string(pos.y * pos.x - pos.y - pos.x);
			auto polygon = rynx::Shape::makeAAOval(0.5f, 40, edgeLength, edgeLength * 0.5f);
			auto* mesh_p = meshes->create(mesh_name, rynx::polygon_triangulation().generate_polygon_boundary(polygon));
			return base_simulation.m_ecs.create(
				rynx::components::position(pos, angle),
				rynx::components::collisions{ collisionCategoryStatic.value },
				rynx::components::boundary({ polygon.generateBoundary_Inside(1.0f) }),
				rynx::components::mesh(mesh_p),
				rynx::matrix4(),
				rynx::components::radius(polygon.radius()),
				rynx::components::color({ 0.5f, 0.2f, 1.0f, 1.0f }),
				rynx::components::motion({ 0, 0, 0 }, angular_velocity),
				rynx::components::physical_body(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0.0f, 1.0f),
				rynx::components::ignore_gravity()
			);
		};

		auto makeBox_outside = [&](rynx::vec3<float> pos, float angle, float edgeLength, float angular_velocity) {
			auto mesh_name = std::to_string(pos.y * pos.x);
			auto polygon = rynx::Shape::makeRectangle(edgeLength, 5.0f);
			auto* mesh_p = meshes->create(mesh_name, rynx::polygon_triangulation().generate_polygon_boundary(polygon));
			float radius = polygon.radius();
			return base_simulation.m_ecs.create(
				rynx::components::position(pos, angle),
				rynx::components::collisions{ collisionCategoryStatic.value },
				rynx::components::boundary({ polygon.generateBoundary_Outside(1.0f) }),
				rynx::components::mesh(mesh_p),
				rynx::matrix4(),
				rynx::components::radius(radius),
				rynx::components::color({ 0.2f, 1.0f, 0.3f, 1.0f }),
				rynx::components::motion({ 0, 0, 0 }, angular_velocity),
				rynx::components::physical_body(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0.0f, 1.0f),
				rynx::components::ignore_gravity(),
				rynx::components::dampening{ 0.50f, 1.0f }
			);
		};

		/*
		for (int i = 0; i < 20; ++i)
			makeBox_inside({ +20, +15 - i * 15.0f, 0 }, 0.0f + i * 0.1f, 2.0f, -0.05f);
		*/

		// makeBox_inside({ -5, -30, 0 }, +0.3f, 40.f, -0.025f);
		makeBox_outside({ -15, -50, 0 }, -0.3f, 265.f, +0.58f);

		// makeBox_inside({ -65, -100, 0 }, 0.f, 60.f, -0.030f);
		makeBox_outside({ -65, -100, 0 }, -0.3f, 65.f, -0.24f);

		// makeBox_inside({ +25, -120, 0 }, +0.5f, 80.f, +0.015f);
		makeBox_outside({ +25, -120, 0 }, -0.3f, 65.f, -0.12f);

		makeBox_outside({ 0, -170, 0 }, -0.0f, 100.0f, 0.f);
		makeBox_outside({ -80, -160, 0 }, -0.3f, 1000.0f, 0.f);
		makeBox_outside({ +80, -160, 0 }, +0.3f, 1000.0f, 0.f);
	}

	// setup some debug controls
	float sleepTime = 0.9f;
	auto slowTime = gameInput.generateAndBindGameKey('X', "slow time");
	auto fastTime = gameInput.generateAndBindGameKey('C', "fast time");

	auto zoomOut = gameInput.generateAndBindGameKey('1', "zoom out");
	auto zoomIn = gameInput.generateAndBindGameKey('2', "zoom in");

	auto menuCamera = std::make_shared<rynx::camera>();

	gameInput.generateAndBindGameKey(gameInput.getMouseKeyPhysical(0), "menuCursorActivation");

	auto cameraUp = gameInput.generateAndBindGameKey('I', "cameraUp");
	auto cameraLeft = gameInput.generateAndBindGameKey('J', "cameraLeft");
	auto cameraRight = gameInput.generateAndBindGameKey('L', "cameraRight");
	auto cameraDown = gameInput.generateAndBindGameKey('K', "cameraDown");

	rynx::menu::Div root({ 1, 1, 0 });

	struct debug_conf {
		bool visualize_dynamic_collisions = false;
		bool visualize_static_collisions = false;
		bool visualize_projectile_collisions = false;
	};

	debug_conf conf;

	// construct menus
	{
		auto sampleButton = std::make_shared<rynx::menu::Button>(*application.textures(), "Frame", &root, rynx::vec3<float>(0.4f, 0.1f, 0), rynx::vec3<float>(), 0.14f);
		auto sampleButton2 = std::make_shared<rynx::menu::Button>(*application.textures(), "Frame", &root, rynx::vec3<float>(0.4f, 0.1f, 0), rynx::vec3<float>(), 0.16f);
		auto sampleButton3 = std::make_shared<rynx::menu::Button>(*application.textures(), "Frame", &root, rynx::vec3<float>(0.4f, 0.1f, 0), rynx::vec3<float>(), 0.18f);
		auto sampleSlider = std::make_shared<rynx::menu::SlideBarVertical>(*application.textures(), "Frame", "Selection", &root, rynx::vec3<float>(0.4f, 0.1f, 0));
		auto megaSlider = std::make_shared<rynx::menu::SlideBarVertical>(*application.textures(), "Frame", "Selection", &root, rynx::vec3<float>(0.4f, 0.1f, 0));

		sampleButton->text("Dynamics").font(&fontConsola);
		sampleButton->alignToInnerEdge(&root, rynx::menu::Align::BOTTOM_LEFT);
		sampleButton->color_frame(Color::RED);
		sampleButton->onClick([&conf, self = sampleButton.get()]() {
			bool new_value = !conf.visualize_dynamic_collisions;
			conf.visualize_dynamic_collisions = new_value;
			if (new_value) {
				self->color_frame(Color::GREEN);
			}
			else {
				self->color_frame(Color::RED);
			}
		});

		sampleButton2->text("Log Profile").font(&fontConsola);
		sampleButton2->alignToOuterEdge(sampleButton.get(), rynx::menu::Align::RIGHT);
		sampleButton2->alignToInnerEdge(sampleButton.get(), rynx::menu::Align::BOTTOM);
		sampleButton2->onClick([]() {
			rynx::profiling::write_profile_log();
		});

		sampleButton3->text("Statics").font(&fontConsola);
		sampleButton3->alignToOuterEdge(sampleButton2.get(), rynx::menu::Align::TOP);
		sampleButton3->alignToInnerEdge(sampleButton2.get(), rynx::menu::Align::LEFT);
		sampleButton3->onClick([&conf, self = sampleButton3.get(), &root]() {
			bool new_value = !conf.visualize_static_collisions;
			conf.visualize_static_collisions = new_value;
			if (new_value) {
				self->color_frame(Color::GREEN);
			}
			else {
				self->color_frame(Color::RED);
			}
		});

		sampleSlider->alignToInnerEdge(&root, rynx::menu::Align::TOP_RIGHT);
		sampleSlider->onValueChanged([](float f) {});

		megaSlider->alignToOuterEdge(sampleSlider.get(), rynx::menu::Align::BOTTOM);
		megaSlider->alignToInnerEdge(sampleSlider.get(), rynx::menu::Align::LEFT);
		megaSlider->onValueChanged([](float f) {});

		root.addChild(sampleButton);
		root.addChild(sampleButton2);
		root.addChild(sampleButton3);
		root.addChild(sampleSlider);
		root.addChild(megaSlider);
	}

	std::atomic<size_t> tickCounter = 0;

	std::atomic<bool> dead_lock_detector_keepalive = true;
	std::thread dead_lock_detector([&]() {
		size_t prev_tick = 994839589;
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		while (dead_lock_detector_keepalive) {
			if (prev_tick == tickCounter && dead_lock_detector_keepalive) {
				scheduler.dump();
				return;
			}
			prev_tick = tickCounter;
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		}
	});

	auto fbo_menu = rynx::graphics::framebuffer::config()
		.set_default_resolution(1920, 1080)
		.add_rgba8_target("color")
		.construct(application.textures(), "menu");

	rynx::graphics::screenspace_draws(); // initialize gpu buffers for screenspace ops.
	rynx::application::renderer render(application, camera);

	auto camera_orientation_key = gameInput.generateAndBindGameKey(gameInput.getMouseKeyPhysical(1), "camera_orientation");

	rynx::timer timer;
	rynx::numeric_property<float> logic_time;
	rynx::numeric_property<float> render_time;
	rynx::numeric_property<float> swap_time;
	rynx::numeric_property<float> total_time;

	audio.open_output_device();

	rynx::timer frame_timer_dt;
	float dt = 1.0f / 120.0f;
	while (!application.isExitRequested()) {
		rynx_profile("Main", "frame");
		frame_timer_dt.reset();

		{
			rynx_profile("Main", "start frame");
			application.startFrame();
		}

		auto mousePos = application.input()->getCursorPosition();
		cameraPosition.tick(dt * 3);

		{
			rynx_profile("Main", "update camera");

			static rynx::vec3f camera_direction(0, 0, 0);

			if (gameInput.isKeyDown(camera_orientation_key)) {
				auto mouseDelta = gameInput.mouseDelta();
				camera_direction += mouseDelta;
			}

			rynx::matrix4 rotator_x;
			rynx::matrix4 rotator_y;
			rotator_x.discardSetRotation(camera_direction.x, 0, 1, 0);
			rotator_y.discardSetRotation(camera_direction.y, -1, 0, 0);

			rynx::vec3f direction = rotator_y * rotator_x * rynx::vec3f(0, 0, -1);

			camera->setPosition(cameraPosition);
			camera->setDirection(direction);
			camera->setProjection(0.02f, 2000.0f, application.aspectRatio());
			camera->rebuild_view_matrix();
		}

		{
			const float camera_translate_multiplier = 400.4f * dt;
			const float camera_zoom_multiplier = (1.0f - dt * 3.0f);
			if (gameInput.isKeyDown(cameraUp)) { cameraPosition += camera->local_forward() * camera_translate_multiplier; }
			if (gameInput.isKeyDown(cameraLeft)) { cameraPosition += camera->local_left() * camera_translate_multiplier; }
			if (gameInput.isKeyDown(cameraRight)) { cameraPosition -= camera->local_left() * camera_translate_multiplier; }
			if (gameInput.isKeyDown(cameraDown)) { cameraPosition -= camera->local_forward() * camera_translate_multiplier; }
			// if (gameInput.isKeyDown(zoomOut)) { cameraPosition *= vec3<float>(1, 1.0f, 1.0f * camera_zoom_multiplier); }
			// if (gameInput.isKeyDown(zoomIn)) { cameraPosition *= vec3<float>(1, 1.0f, 1.0f / camera_zoom_multiplier); }
		}

		{
			float cameraHeight = cameraPosition->z;
			gameInput.mouseWorldPosition(
			(cameraPosition * rynx::vec3<float>{1, 1, 0}) +
				mousePos * rynx::vec3<float>(cameraHeight, cameraHeight / application.aspectRatio(), 1.0f)
			);
		}

		{
			rynx_profile("Main", "Input handling");
			// TODO: Simulation API
			std::vector<std::unique_ptr<rynx::application::logic::iaction>> userActions = base_simulation.m_logic.onInput(gameInput, ecs);
			for (auto&& action : userActions) {
				action->apply(ecs);
			}
		}

		timer.reset();
		{
			rynx_profile("Main", "Construct frame tasks");
			base_simulation.generate_tasks(dt);
		}

		{
			rynx_profile("Main", "Start scheduler");
			scheduler.start_frame();
		}

		{
			rynx_profile("Main", "Wait for frame end");
			scheduler.wait_until_complete();
			++tickCounter;
		}

		auto logic_time_us = timer.time_since_last_access_us();
		logic_time.observe_value(logic_time_us / 1000.0f); // down to milliseconds.

		// menu input is part of logic, not visualization. must tick every frame.
		root.input(gameInput);
		root.tick(dt, application.aspectRatio());

		// should we render or not.
		if (true) {
			timer.reset();
			rynx_profile("Main", "graphics");

			{
				rynx_profile("Main", "prepare");
				render.prepare(base_simulation.m_context);
				scheduler.start_frame();

				// while waiting for computing to be completed, draw menus.
				{
					rynx_profile("Main", "Menus");
					fbo_menu->bind_as_output();
					fbo_menu->clear();

					application.meshRenderer().setDepthTest(false);

					// 2, 2 is the size of the entire screen (in case of 1:1 aspect ratio) for menu camera. left edge is [-1, 0], top right is [+1, +1], etc.
					// so we make it size 2,2 to cover all of that. and then take aspect ratio into account by dividing the y-size.
					root.scale_local({ 2 , 2 / application.aspectRatio(), 0 });
					menuCamera->setProjection(0.01f, 50.0f, application.aspectRatio());
					menuCamera->setPosition({ 0, 0, 1 });
					menuCamera->rebuild_view_matrix();

					application.meshRenderer().setCamera(menuCamera);
					application.textRenderer().setCamera(menuCamera);
					application.meshRenderer().cameraToGPU();
					root.visualise(application.meshRenderer(), application.textRenderer());

					auto num_entities = ecs.size();
					float info_text_pos_y = +0.1f;
					auto get_min_avg_max = [](rynx::numeric_property<float>& prop) {
						return std::to_string(prop.min()) + "/" + std::to_string(prop.avg()) + "/" + std::to_string(prop.max()) + "ms";
					};

					application.textRenderer().drawText(std::string("logic:    ") + get_min_avg_max(logic_time), -0.9f, 0.40f + info_text_pos_y, 0.05f, Color::DARK_GREEN, rynx::TextRenderer::Align::Left, fontConsola);
					application.textRenderer().drawText(std::string("draw:     ") + get_min_avg_max(render_time), -0.9f, 0.35f + info_text_pos_y, 0.05f, Color::DARK_GREEN, rynx::TextRenderer::Align::Left, fontConsola);
					application.textRenderer().drawText(std::string("swap:     ") + get_min_avg_max(swap_time), -0.9f, 0.30f + info_text_pos_y, 0.05f, Color::DARK_GREEN, rynx::TextRenderer::Align::Left, fontConsola);
					application.textRenderer().drawText(std::string("total:    ") + get_min_avg_max(total_time), -0.9f, 0.25f + info_text_pos_y, 0.05f, Color::DARK_GREEN, rynx::TextRenderer::Align::Left, fontConsola);
					application.textRenderer().drawText(std::string("bodies:   ") + std::to_string(ecs.query().in<rynx::components::physical_body>().count()), -0.9f, 0.20f + info_text_pos_y, 0.05f, Color::DARK_GREEN, rynx::TextRenderer::Align::Left, fontConsola);
					application.textRenderer().drawText(std::string("entities: ") + std::to_string(num_entities), -0.9f, 0.15f + info_text_pos_y, 0.05f, Color::DARK_GREEN, rynx::TextRenderer::Align::Left, fontConsola);
					application.textRenderer().drawText(std::string("frustum culled: ") + std::to_string(ecs.query().in<rynx::components::frustum_culled>().count()), -0.9f, 0.10f + info_text_pos_y, 0.05f, Color::DARK_GREEN, rynx::TextRenderer::Align::Left, fontConsola);
					application.textRenderer().drawText(std::string("visible: ") + std::to_string(ecs.query().notIn<rynx::components::frustum_culled>().count()), -0.9f, 0.05f + info_text_pos_y, 0.05f, Color::DARK_GREEN, rynx::TextRenderer::Align::Left, fontConsola);
				}

				scheduler.wait_until_complete();
			}

			auto render_time_us = timer.time_since_last_access_us();
			render_time.observe_value(render_time_us / 1000.0f);

			auto mpos = gameInput.mouseWorldPosition();
			{
				rynx_profile("Main", "draw cursor");

				rynx::matrix4 m;
				m.discardSetTranslate(mpos);
				m.scale(0.5f);
				application.meshRenderer().drawMesh(*meshes->get("circle_empty"), m, "Empty");
			}

			{
				rynx_profile("Main", "draw");

				render.execute();

				// TODO: debug visualisations should be drawn on their own fbo.
				application.debugVis()->prepare(base_simulation.m_context);
				{
					// visualize collision detection structure.
					if (conf.visualize_dynamic_collisions) {
						std::array<rynx::vec4<float>, 5> node_colors{ rynx::vec4<float>{0, 1, 0, 0.2f}, {0, 0, 1, 0.2f}, {1, 0, 0, 0.2f}, {1, 1, 0, 0.2f}, {0, 1, 1, 0.2f} };
						collisionDetection.get(collisionCategoryDynamic)->forEachNode([&](rynx::vec3<float> pos, float radius, int depth) {
							rynx::matrix4 m;
							m.discardSetTranslate(pos);
							m.scale(radius);
							float sign[2] = { -1.0f, +1.0f };
							application.debugVis()->addDebugVisual(meshes->get("circle_empty"), m, node_colors[depth % node_colors.size()]);
						});
					}

					// visualize collision detection structure.
					if (conf.visualize_static_collisions) {
						std::array<rynx::vec4<float>, 5> node_colors{ rynx::vec4<float>{0, 1, 0, 0.2f}, {0, 0, 1, 0.2f}, {1, 0, 0, 0.2f}, {1, 1, 0, 0.2f}, {0, 1, 1, 0.2f} };
						collisionDetection.get(collisionCategoryStatic)->forEachNode([&](rynx::vec3<float> pos, float radius, int depth) {
							rynx::matrix4 m;
							m.discardSetTranslate(pos);
							m.scale(radius);
							float sign[2] = { -1.0f, +1.0f };
							application.debugVis()->addDebugVisual(meshes->get("circle_empty"), m, node_colors[depth % node_colors.size()]);
						});
					}
				}

				{
					application.debugVis()->execute();
				}

				{
					application.shaders()->activate_shader("fbo_color_to_bb");
					fbo_menu->bind_as_input();
					rynx::graphics::screenspace_draws::draw_fullscreen();
				}

				timer.reset();
				application.swapBuffers();
				auto swap_time_us = timer.time_since_last_access_us();
				swap_time.observe_value(swap_time_us / 1000.0f);

				total_time.observe_value((logic_time_us + render_time_us + swap_time_us) / 1000.0f);
			}

			ecs.query().for_each([&ecs](rynx::components::position& pos, rynx::components::position_relative relative_pos) {
				const auto& host_pos = ecs[relative_pos.host].get<rynx::components::position>();
				pos.value = host_pos.value + rynx::math::rotatedXY(relative_pos.relative_pos, host_pos.angle);
			});
		}

		{
			rynx_profile("Main", "Clean up dead entitites");
			dt = std::min(0.016f, std::max(0.001f, frame_timer_dt.time_since_last_access_ms() * 0.001f));

			{
				std::vector<rynx::ecs::id> ids;
				ecs.query().for_each([&ids, dt](rynx::ecs::id id, rynx::components::lifetime& time) {
					time.value -= dt;
					if (time.value <= 0) {
						ids.emplace_back(id);
					}
				});
				for (auto&& id : ids)
					ecs.attachToEntity(id, rynx::components::dead());
			}

			auto ids_dead = ecs.query().in<rynx::components::dead>().ids();

			for (auto id : ids_dead) {
				if (ecs[id].has<rynx::components::collisions>()) {
					auto collisions = ecs[id].get<rynx::components::collisions>();
					collisionDetection.erase(id.value, collisions.category);
				}
			}

			base_simulation.m_logic.entities_erased(*base_simulation.m_context, ids_dead);
			ecs.erase(ids_dead);
			if (!ids_dead.empty()) {
				logmsg("erased entities: %d", ids_dead.size());
			}
		}

		{
			rynx_profile("Main", "Sleep");
			// NOTE: Frame time can be edited during runtime for debugging reasons.
			if (gameInput.isKeyDown(slowTime)) { sleepTime *= 1.1f; }
			if (gameInput.isKeyDown(fastTime)) { sleepTime *= 0.9f; }

			std::this_thread::sleep_for(std::chrono::milliseconds(int(sleepTime)));
		}
	}

	dead_lock_detector_keepalive = false;
	dead_lock_detector.join();
	return 0;
}
