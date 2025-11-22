#include <algorithm>
#include <csignal>
#include <ctime>
#include <string>
#include <thread>
#include <chrono>
#include <optional>
#include <memory>
#include <fmt/core.h>
#include <fmt/ostream.h>
#include <magic_enum.hpp>

#include "bridge.hpp"
#include "version.h"
#include <ProtobufMessages.pb.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#if defined(WIN32)
#include <Windows.h>
#endif

#define XR_PRINT(e) { \
	XrResult res = (e); \
	if (res != XR_SUCCESS) { \
		fmt::println("OpenXR call (" #e ") returned {}", magic_enum::enum_name(res)); \
	} \
}
#define XR_CHECK(e) { \
	char buf[64]; \
	XrResult res = (e); \
	if (res < XR_SUCCESS) { \
		fmt::println(#e " failed: {}", magic_enum::enum_name(res)); \
		throw std::runtime_error(fmt::format("OpenXR call " #e " returned {}", magic_enum::enum_name(res))); \
	} \
}

constexpr XrPosef IDENTITY_POSE{
	.orientation = {.w = 1.f},
	.position = {},
};

XrTime now(XrInstance instance)
{
	static PFN_xrConvertTimespecTimeToTimeKHR xrConvertTimespecTimeToTimeKHR = nullptr;
	if (!xrConvertTimespecTimeToTimeKHR)
		XR_CHECK(xrGetInstanceProcAddr(instance, "xrConvertTimeToTimespecTimeKHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrConvertTimespecTimeToTimeKHR)));

	timespec now;
	XrTime out;
	clock_gettime(CLOCK_MONOTONIC, &now);
	XR_CHECK(xrConvertTimespecTimeToTimeKHR(instance, &now, &out));
	return out;
}

enum class BodyPosition {
	Head = 0,
	LeftHand,
	RightHand,
	LeftFoot,
	RightFoot,
	LeftShoulder,
	RightShoulder,
	LeftElbow,
	RightElbow,
	LeftKnee,
	RightKnee,
	Waist,
	Chest,
	BodyPosition_Count
};

// TODO: keep track of things as SlimeVRPosition in the first place.
enum class SlimeVRPosition {
	None = 0,
	Waist,
	LeftFoot,
	RightFoot,
	Chest,
	LeftKnee,
	RightKnee,
	LeftElbow,
	RightElbow,
	LeftShoulder,
	RightShoulder,
	LeftHand,
	RightHand,
	LeftController,
	RightController,
	Head,
	Neck,
	Camera,
	Keyboard,
	HMD,
	Beacon,
	GenericController
};

static constexpr SlimeVRPosition positionIDs[(int)BodyPosition::BodyPosition_Count] = {
	SlimeVRPosition::Head,
	SlimeVRPosition::LeftController,
	SlimeVRPosition::RightController,
	SlimeVRPosition::LeftFoot,
	SlimeVRPosition::RightFoot,
	SlimeVRPosition::LeftShoulder,
	SlimeVRPosition::RightShoulder,
	SlimeVRPosition::LeftElbow,
	SlimeVRPosition::RightElbow,
	SlimeVRPosition::LeftKnee,
	SlimeVRPosition::RightKnee,
	SlimeVRPosition::Waist,
	SlimeVRPosition::Chest
};

enum class TrackerState {
	DISCONNECTED,
	WAITING,
	RUNNING
};

struct TrackerInfo {
	std::string name = "";
	std::optional<std::string> serial = std::nullopt;
	SlimeVRPosition position = SlimeVRPosition::None;
	messages::TrackerStatus_Status status = messages::TrackerStatus_Status_DISCONNECTED;

	TrackerState state = TrackerState::DISCONNECTED;
	/// number of ticks since last detect or valid pose
	uint8_t connection_timeout = 0;
	/// number of ticks since NONE position was first detected
	uint8_t detect_timeout = 0;
};

std::atomic_bool should_exit{ false };

void handle_signal(int num) {
	// reinstall, in case it goes back to default.
	//signal(num, handle_signal);
	switch (num) {
	case SIGINT:
		should_exit = true;
		break;
	}
}

void send_tracker_added(SlimeVRBridge & bridge, int32_t id, SlimeVRPosition role, const std::string & name, const std::string & serial) {
	fmt::println("send_tracker_added({}, {}, {}, {})", id, magic_enum::enum_name(role), name, serial);
	messages::ProtobufMessage msg;
	messages::TrackerAdded * added = msg.mutable_tracker_added();
	added->set_tracker_id(id);
	added->set_tracker_role(static_cast<int>(role));
	added->set_tracker_name(name);
	added->set_tracker_serial(serial);
	bridge.sendMessage(msg);
}

void send_tracker_status(SlimeVRBridge & bridge, int32_t id, messages::TrackerStatus_Status status) {
	messages::ProtobufMessage msg;
	messages::TrackerStatus * status_msg = msg.mutable_tracker_status();
	status_msg->set_tracker_id(id);
	status_msg->set_status(status);
	bridge.sendMessage(msg);
}

void send_tracker_position(SlimeVRBridge & bridge, int32_t id, XrSpaceLocation location) {
	messages::ProtobufMessage msg;
	messages::Position * position = msg.mutable_position();
	position->set_tracker_id(id);
	position->set_x(location.pose.position.x);
	position->set_y(location.pose.position.y);
	position->set_z(location.pose.position.z);
	position->set_qx(location.pose.orientation.x);
	position->set_qy(location.pose.orientation.y);
	position->set_qz(location.pose.orientation.z);
	position->set_qw(location.pose.orientation.w);
	position->set_data_source(messages::Position_DataSource_FULL);
	bridge.sendMessage(msg);
}

constexpr std::array required_extensions = {XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, XR_MND_HEADLESS_EXTENSION_NAME};
constexpr std::array interaction_profiles = {"/interaction_profiles/khr/simple_controller", "/interaction_profiles/oculus/touch_controller", "/interaction_profiles/valve/index_controller"};

int main(int argc, char* argv[]) {
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	fmt::print("SlimeVR-Feeder-App version {}\n\n", version);

	signal(SIGINT, handle_signal);

#if defined(WIN32)
	// Hide command line output after any potential error
	if (!show_console) {
		// Thanks https://stackoverflow.com/a/78943791
		HWND console = GetConsoleWindow();
		HWND console_owner = GetWindow(console, GW_OWNER);
		if (console_owner == NULL) {
		    ShowWindow(console, SW_HIDE);
		}
		else {
		    ShowWindow(console_owner, SW_HIDE); // Windows Terminal
		}
	}
#endif

	auto bridge = SlimeVRBridge::factory();
	
	XrInstance instance;
	XrSystemId system;
	XrSession session;

	XrActionSet action_set;
	XrSpace left_grip, right_grip;

	// init
	{
		uint32_t property_count;
		XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &property_count, nullptr));
		
		std::vector<XrExtensionProperties> properties(property_count, {
			.type = XR_TYPE_EXTENSION_PROPERTIES,
		});
		XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, property_count, &property_count, properties.data()));

		if (!std::ranges::all_of(required_extensions, [&properties](auto ext_name) {
			return std::find_if(properties.cbegin(), properties.cend(), [ext_name](const auto & ext) {
				return strcmp(ext.extensionName, ext_name) == 0;
			}) != properties.cend();
		})) {
			fmt::println("Required extension not found");
			return 1;
		}

		const XrInstanceCreateInfo instance_create_info{
			.type = XR_TYPE_INSTANCE_CREATE_INFO,
			.applicationInfo = {
				.applicationName = "SlimeVR Feeder App",
				.applicationVersion = 0,
				.apiVersion = XR_API_VERSION_1_0,
			},
			.enabledExtensionCount = required_extensions.size(),
			.enabledExtensionNames = required_extensions.data(),
		};
		XR_CHECK(xrCreateInstance(&instance_create_info, &instance));

		const XrSystemGetInfo system_get_info{
			.type = XR_TYPE_SYSTEM_GET_INFO,
			.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
		};
		XR_CHECK(xrGetSystem(instance, &system_get_info, &system));

		const XrSessionCreateInfo session_create_info{
			.type = XR_TYPE_SESSION_CREATE_INFO,
			.systemId = system,
		};
		XR_CHECK(xrCreateSession(instance, &session_create_info, &session));

		// create hand pose actions
		const XrActionSetCreateInfo action_set_create_info{
			.type = XR_TYPE_ACTION_SET_CREATE_INFO,
			.actionSetName = "hands",
			.localizedActionSetName = "Hands",
			.priority = 0,
		};
		XR_CHECK(xrCreateActionSet(instance, &action_set_create_info, &action_set));

		XrAction left_pose_action, right_pose_action;
		const XrActionCreateInfo left_action_create_info{
			.type = XR_TYPE_ACTION_CREATE_INFO,
			.actionName = "left_hand_pose",
			.actionType = XR_ACTION_TYPE_POSE_INPUT,
			.localizedActionName = "Left Hand Pose",
		};
		const XrActionCreateInfo right_action_create_info{
			.type = XR_TYPE_ACTION_CREATE_INFO,
			.actionName = "right_hand_pose",
			.actionType = XR_ACTION_TYPE_POSE_INPUT,
			.localizedActionName = "Right Hand Pose",
		};

		XR_CHECK(xrCreateAction(action_set, &left_action_create_info, &left_pose_action));
		XR_CHECK(xrCreateAction(action_set, &right_action_create_info, &right_pose_action));

		// create spaces
		XrActionSpaceCreateInfo action_space_create_info{
			.type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
			.subactionPath = XR_NULL_PATH,
			.poseInActionSpace = IDENTITY_POSE,
		};
		
		action_space_create_info.action = left_pose_action;
		XR_CHECK(xrCreateActionSpace(session, &action_space_create_info, &left_grip));
		action_space_create_info.action = right_pose_action;
		XR_CHECK(xrCreateActionSpace(session, &action_space_create_info, &right_grip));

		// suggest bindings
		XrPath left_pose_path, right_pose_path;
		XR_CHECK(xrStringToPath(instance, "/user/hand/left/input/grip/pose", &left_pose_path));
		XR_CHECK(xrStringToPath(instance, "/user/hand/right/input/grip/pose", &right_pose_path));

		XrActionSuggestedBinding bindings[] = {
			{.action = left_pose_action, .binding = left_pose_path},
			{.action = right_pose_action, .binding = right_pose_path},
		};

		XrInteractionProfileSuggestedBinding suggested_bindings{
			.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
			.countSuggestedBindings = std::size(bindings),
			.suggestedBindings = bindings,
		};

		for (auto profile : interaction_profiles) {
			XrPath profile_path;
			XR_CHECK(xrStringToPath(instance, profile, &profile_path));

			suggested_bindings.interactionProfile = profile_path;
			XR_CHECK(xrSuggestInteractionProfileBindings(instance, &suggested_bindings));
		}

		XrSessionActionSetsAttachInfo attach_info{
			.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
			.countActionSets = 1,
			.actionSets = &action_set,
		};
		XR_CHECK(xrAttachSessionActionSets(session, &attach_info));
	}

	XrSpace view, stage;
	{
		XrReferenceSpaceCreateInfo space_create_info{
			.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
			.poseInReferenceSpace = IDENTITY_POSE,
		};

		space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		XR_CHECK(xrCreateReferenceSpace(session, &space_create_info, &view));

		space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
		XR_CHECK(xrCreateReferenceSpace(session, &space_create_info, &stage));
	}

	auto tick_ns = std::chrono::nanoseconds(1'000'000'000 / 200);
	auto next_tick = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::high_resolution_clock::now().time_since_epoch()
	);

	bool session_running = false;
	// event loop
	while (true) {
		bridge->runFrame();

		XrEventDataBuffer buffer;
		while (xrPollEvent(instance, &buffer) == XR_SUCCESS)
		{
			switch (buffer.type)
			{
				case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
					const auto & event = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&buffer);
					if (event.session != session)
					{
						fmt::println("Got SESSION_STATE_CHANGED for unknown session");
						break;
					}

					fmt::println("Session state changed to {}", magic_enum::enum_name(event.state));
					switch (event.state)
					{
						case XR_SESSION_STATE_READY: {
							const XrSessionBeginInfo session_begin_info{
								.type = XR_TYPE_SESSION_BEGIN_INFO,
								.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
							};
							XR_CHECK(xrBeginSession(session, &session_begin_info));
							session_running = true;
							send_tracker_added(*bridge, 0, SlimeVRPosition::HMD, "HMD", "OpenXR HMD");
							send_tracker_added(*bridge, 1, SlimeVRPosition::LeftHand, "Left Hand", "OpenXR Left Hand");
							send_tracker_added(*bridge, 2, SlimeVRPosition::RightHand, "Right Hand", "OpenXR Right Hand");
							break;
						}
						case XR_SESSION_STATE_STOPPING:
						case XR_SESSION_STATE_LOSS_PENDING:
							XR_PRINT(xrDestroySpace(view));
							XR_PRINT(xrDestroySpace(stage));
							session_running = false;
							XR_PRINT(xrEndSession(session));
							break;
						case XR_SESSION_STATE_EXITING:
							goto exit;
						default: break;
					}
				}
				default: break;
			}
		}

		messages::ProtobufMessage recievedMessage;
		// TODO: I don't think there are any messages from the server that we care about at the moment, but let's make sure to not let the pipe fill up.
		bridge->getNextMessage(recievedMessage);

		if (session_running) {
			if (should_exit) {
				fmt::println("Requesting quit");
				XR_PRINT(xrRequestExitSession(session));
				should_exit = false;
			}

			XrActiveActionSet active_action_set{
				.actionSet = action_set,
				.subactionPath = XR_NULL_PATH,
			};
			XrActionsSyncInfo actions_sync_info{
				.type = XR_TYPE_ACTIONS_SYNC_INFO,
				.countActiveActionSets = 1,
				.activeActionSets = &active_action_set,
			};
			XR_CHECK(xrSyncActions(session, &actions_sync_info));

			XrTime now_t = now(instance);
			XrSpaceLocation view_location{
				.type = XR_TYPE_SPACE_LOCATION,
			};
			XR_CHECK(xrLocateSpace(view, stage, now_t, &view_location));

			XrSpaceLocation left_hand_location{
				.type = XR_TYPE_SPACE_LOCATION,
			};
			XR_CHECK(xrLocateSpace(left_grip, stage, now_t, &left_hand_location));

			XrSpaceLocation right_hand_location{
				.type = XR_TYPE_SPACE_LOCATION,
			};
			XR_CHECK(xrLocateSpace(right_grip, stage, now_t, &right_hand_location));

			if ((view_location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) == 0)
				send_tracker_status(*bridge, 0, messages::TrackerStatus_Status_OCCLUDED);
			else
				send_tracker_status(*bridge, 0, messages::TrackerStatus_Status_OK);

			if ((left_hand_location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) == 0)
				send_tracker_status(*bridge, 1, messages::TrackerStatus_Status_OCCLUDED);
			else
				send_tracker_status(*bridge, 1, messages::TrackerStatus_Status_OK);

			if ((right_hand_location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) == 0)
				send_tracker_status(*bridge, 2, messages::TrackerStatus_Status_OCCLUDED);
			else
				send_tracker_status(*bridge, 2, messages::TrackerStatus_Status_OK);

			send_tracker_position(*bridge, 0, view_location);
			send_tracker_position(*bridge, 1, left_hand_location);
			send_tracker_position(*bridge, 2, right_hand_location);
		}

		next_tick += tick_ns;

		auto wait_ns = next_tick - std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()
		);

		if (wait_ns.count() > 0) {
			std::this_thread::sleep_for(wait_ns);
		} else {
			// I don't care if you want more TPS than the feeder can provide, I'm yielding to the OS anyway.
			// if this is really an issue for someone, they can open an issue.
			std::this_thread::yield();
		}
	}

exit:
	XR_PRINT(xrDestroySpace(left_grip));
	XR_PRINT(xrDestroySpace(right_grip));
	XR_PRINT(xrDestroySession(session));
	XR_PRINT(xrDestroyInstance(instance));
	fmt::print("Exiting cleanly!\n");

	return 0;
}
