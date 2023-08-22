#include "viewer.hpp"

struct push_constants {
	glm::mat4 model;
	glm::mat4 view;
	glm::mat4 proj;
};

static const char *vertex_shader = R"(
#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;

layout (push_constant) uniform VertexPushConstants {
	mat4 model;
	mat4 view;
	mat4 proj;
};

layout (location = 0) out vec3 out_normal;

void main()
{
	gl_Position = proj * view * model * vec4(position, 1.0);
	gl_Position.y = -gl_Position.y;
	gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0;
	out_normal = mat3(transpose(inverse(model))) * normal;
}
)";

static const char *shaded_fragment_shader = R"(
#version 450

layout (location = 0) in vec3 in_normal;

layout (location = 0) out vec4 fragment;

void main()
{
	vec3 light_direction = normalize(vec3(1.0, 1.0, 1.0));
	float light_intensity = max(0.0, dot(in_normal, light_direction));
	vec3 color = vec3(light_intensity + 0.1);
	fragment = vec4(color, 1.0);
}
)";


static const char *normal_fragment_shader = R"(
#version 450

layout (location = 0) in vec3 in_normal;

layout (location = 0) out vec4 fragment;

void main()
{
	fragment = vec4(in_normal * 0.5 + 0.5, 1.0);
}
)";

static const char *transparent_fragment_shader = R"(
#version 450

layout (location = 0) out vec4 fragment;

void main()
{
	fragment = vec4(1.0, 0.5, 0.5, 0.5);
}
)";

struct wireframe_push_constants {
	glm::mat4 model;
	glm::mat4 view;
	glm::mat4 proj;
	glm::vec3 color;
};

static const char *wireframe_fragment_shader = R"(
#version 450

layout (push_constant) uniform FragmentPushConstants {
	layout (offset = 192) vec3 color;
};

layout (location = 0) out vec4 fragment;

void main()
{
	fragment = vec4(color, 1.0);
}
)";

static const char *face_vertex_shader = R"(
#version 450

layout (location = 0) in vec3 position;
layout (location = 1) in vec3 normal;
layout (location = 2) in vec3 color;

layout (push_constant) uniform VertexPushConstants {
	mat4 model;
	mat4 view;
	mat4 proj;
};

layout (location = 0) out vec3 out_color;

void main()
{
	gl_Position = proj * view * model * vec4(position, 1.0);
	gl_Position.y = -gl_Position.y;
	gl_Position.z = (gl_Position.z + gl_Position.w) / 2.0;

	out_color = color;
}
)";

static const char *point_color_fragment_shader = R"(
#version 450

layout (location = 0) in vec3 color;

layout (location = 0) out vec4 fragment;

void main()
{
	fragment = vec4(color, 1.0);
}
)";

// Vertex properties
static constexpr vk::VertexInputBindingDescription vertex_binding {
	0, 2 * sizeof(glm::vec3), vk::VertexInputRate::eVertex
};

static constexpr std::array <vk::VertexInputAttributeDescription, 2> vertex_attributes {
	vk::VertexInputAttributeDescription {
		0, 0, vk::Format::eR32G32B32Sfloat, 0
	},
	vk::VertexInputAttributeDescription {
		1, 0, vk::Format::eR32G32B32Sfloat, sizeof(glm::vec3)
	},
};

static constexpr vk::VertexInputBindingDescription perface_vertex_binding {
	0, 3 * sizeof(glm::vec3), vk::VertexInputRate::eVertex
};

static constexpr std::array <vk::VertexInputAttributeDescription, 3> perface_vertex_attributes {
	vk::VertexInputAttributeDescription {
		0, 0, vk::Format::eR32G32B32Sfloat, 0
	},
	vk::VertexInputAttributeDescription {
		1, 0, vk::Format::eR32G32B32Sfloat, sizeof(glm::vec3)
	},
	vk::VertexInputAttributeDescription {
		2, 0, vk::Format::eR32G32B32Sfloat, 2 * sizeof(glm::vec3)
	},
};

// Mouse handling
struct {
	float last_x = 0.0f;
	float last_y = 0.0f;

	bool dragging = false;

	decltype(Viewer::camera) *camera = nullptr;
} mouse;

void mouse_button_callback(GLFWwindow *window, int button, int action, int mods)
{
	// Check if mouse is in an ImGui window
	ImGuiIO &io = ImGui::GetIO();
	io.AddMouseButtonEvent(button, action);

	bool mouse_in_window = io.WantCaptureMouse;

	if (button == GLFW_MOUSE_BUTTON_LEFT)
		mouse.dragging = (action == GLFW_PRESS) && !mouse_in_window;
}

void mouse_position_callback(GLFWwindow *window, double xpos, double ypos)
{
	constexpr float sensitivity = 0.05f;

	ImGuiIO &io = ImGui::GetIO();

	if (mouse.dragging) {
		float dx = xpos - mouse.last_x;
		float dy = ypos - mouse.last_y;

		glm::vec2 delta = { dy, -dx };
		mouse.camera->rotate(delta * sensitivity);
	} else {
		io.AddMousePosEvent(xpos, ypos);
	}

	mouse.last_x = xpos;
	mouse.last_y = ypos;
}

// Viewer camera controls
glm::mat4 Camera::proj(const vk::Extent2D &ext) const
{
	return glm::perspective(
		glm::radians(fov),
		(float) ext.width / (float) ext.height,
		0.1f, 1e5f
	);
}

glm::mat4 Camera::view() const
{
	glm::quat q = glm::quat(glm::radians(rotation));
	glm::mat4 rot = glm::mat4_cast(q);
	glm::mat4 mat = glm::translate(glm::mat4(1.0f), position);
	glm::mat4 view = rot * mat;

	constexpr glm::vec4 up = { 0, 1, 0, 0 };
	constexpr glm::vec4 forward = { 0, 0, 1, 0 };

	glm::vec3 l_forward = glm::normalize(glm::vec3(view * forward));
	glm::vec3 l_up = glm::normalize(glm::vec3(view * up));

	return glm::lookAt(position, position + l_forward, l_up);
}

void Camera::move(const glm::vec3 &delta)
{
	glm::quat q = glm::quat(glm::radians(rotation));
	glm::mat4 rot = glm::mat4_cast(q);
	glm::mat4 mat = glm::translate(glm::mat4(1.0f), position);
	glm::mat4 view = rot * mat;
	glm::vec3 l_delta = glm::vec3(view * glm::vec4(delta, 0.0f));
	position += l_delta;
}

void Camera::rotate(const glm::vec2 &delta)
{
	pitch += delta.x;
	yaw += delta.y;

	constexpr float limit = 89.0f;
	pitch = glm::clamp(pitch, -limit, limit);

	rotation = { pitch, yaw, 0 };
}

// Constructor loads a device and starts the initialization process
Viewer::Viewer()
{
	// Load Vulkan physical device
	auto predicate = [](const vk::PhysicalDevice &dev) {
		return littlevk::physical_device_able(dev, {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		});
	};

	// Initialize the window and resources
	vk::PhysicalDevice dev = littlevk::pick_physical_device(predicate);
	skeletonize(dev, { 2560, 1440 }, "Viewer");
	from(dev);

	// Setup callbacks
	glfwSetMouseButtonCallback(window->handle, mouse_button_callback);
	glfwSetCursorPosCallback(window->handle, mouse_position_callback);
	mouse.camera = &camera;
}

// Initialize the viewer
void Viewer::from(const vk::PhysicalDevice &phdev_)
{
	// Copy the physical device and properties
	phdev = phdev_;
	mem_props = phdev.getMemoryProperties();

	// Configure basic resources
	dal = new littlevk::Deallocator(device);

	// Create the render pass
	std::array <vk::AttachmentDescription, 2> attachments {
		littlevk::default_color_attachment(swapchain.format),
		littlevk::default_depth_attachment(),
	};

	std::array <vk::AttachmentReference, 1> color_attachments {
		vk::AttachmentReference {
			0, vk::ImageLayout::eColorAttachmentOptimal,
		}
	};

	vk::AttachmentReference depth_attachment {
		1, vk::ImageLayout::eDepthStencilAttachmentOptimal,
	};

	vk::SubpassDescription subpass {
		{}, vk::PipelineBindPoint::eGraphics,
		{}, color_attachments,
		{}, &depth_attachment,
	};

	render_pass = littlevk::render_pass(
		device,
		vk::RenderPassCreateInfo {
			{}, attachments, subpass
		}
	).unwrap(dal);

	// Create a depth buffer
	littlevk::ImageCreateInfo depth_info {
		window->extent.width,
		window->extent.height,
		vk::Format::eD32Sfloat,
		vk::ImageUsageFlagBits::eDepthStencilAttachment,
		vk::ImageAspectFlagBits::eDepth,
	};

	littlevk::Image depth_buffer = littlevk::image(
		device,
		depth_info,
		mem_props
	).unwrap(dal);

	// Create framebuffers from the swapchain
	littlevk::FramebufferSetInfo fb_info;
	fb_info.swapchain = &swapchain;
	fb_info.render_pass = render_pass;
	fb_info.extent = window->extent;
	fb_info.depth_buffer = &depth_buffer.view;

	framebuffers = littlevk::framebuffers(device, fb_info).unwrap(dal);

	// Allocate command buffers
	command_pool = littlevk::command_pool(device,
		vk::CommandPoolCreateInfo {
			vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			littlevk::find_graphics_queue_family(phdev)
		}
	).unwrap(dal);

	command_buffers = device.allocateCommandBuffers({
		command_pool, vk::CommandBufferLevel::ePrimary, 2
	});

	// Compile shader modules
	vk::ShaderModule vertex_module = littlevk::shader::compile(
		device, std::string(vertex_shader),
		vk::ShaderStageFlagBits::eVertex
	).unwrap(dal);

	vk::ShaderModule face_vertex_module = littlevk::shader::compile(
		device, std::string(face_vertex_shader),
		vk::ShaderStageFlagBits::eVertex
	).unwrap(dal);

	vk::ShaderModule shaded_fragment_module = littlevk::shader::compile(
		device, std::string(shaded_fragment_shader),
		vk::ShaderStageFlagBits::eFragment
	).unwrap(dal);

	vk::ShaderModule normal_fragment_module = littlevk::shader::compile(
		device, std::string(normal_fragment_shader),
		vk::ShaderStageFlagBits::eFragment
	).unwrap(dal);

	vk::ShaderModule transparent_fragment_module = littlevk::shader::compile(
		device, std::string(transparent_fragment_shader),
		vk::ShaderStageFlagBits::eFragment
	).unwrap(dal);

	vk::ShaderModule wireframe_fragment_module = littlevk::shader::compile(
		device, std::string(wireframe_fragment_shader),
		vk::ShaderStageFlagBits::eFragment
	).unwrap(dal);

	vk::ShaderModule point_color_fragment_module = littlevk::shader::compile(
		device, std::string(point_color_fragment_shader),
		vk::ShaderStageFlagBits::eFragment
	).unwrap(dal);

	// Create the pipeline
	vk::PushConstantRange push_constant_range {
		vk::ShaderStageFlagBits::eVertex,
		0, sizeof(push_constants)
	};

	littlevk::pipeline::GraphicsCreateInfo pipeline_info;
	pipeline_info.vertex_binding = vertex_binding;
	pipeline_info.vertex_attributes = vertex_attributes;
	pipeline_info.vertex_shader = vertex_module;
	pipeline_info.extent = window->extent;
	pipeline_info.render_pass = render_pass;
	pipeline_info.cull_mode = vk::CullModeFlagBits::eNone;


	{
		vk::PipelineLayout pipeline_layout = littlevk::pipeline_layout(
			device,
			vk::PipelineLayoutCreateInfo {
				{}, nullptr, push_constant_range
			}
		).unwrap(dal);

		pipeline_info.fragment_shader = shaded_fragment_module;
		pipeline_info.pipeline_layout = pipeline_layout;
		pipeline_info.alpha_blend = false;

		pipelines[0].first = pipeline_layout;
		pipelines[0].second = littlevk::pipeline::compile(device, pipeline_info).unwrap(dal);
	}

	{
		vk::PipelineLayout pipeline_layout = littlevk::pipeline_layout(
			device,
			vk::PipelineLayoutCreateInfo {
				{}, nullptr, push_constant_range
			}
		).unwrap(dal);

		pipeline_info.fragment_shader = normal_fragment_module;
		pipeline_info.pipeline_layout = pipeline_layout;
		pipeline_info.alpha_blend = false;

		pipelines[1].first = pipeline_layout;
		pipelines[1].second = littlevk::pipeline::compile(device, pipeline_info).unwrap(dal);
	}

	{
		vk::PipelineLayout pipeline_layout = littlevk::pipeline_layout(
			device,
			vk::PipelineLayoutCreateInfo {
				{}, nullptr, push_constant_range
			}
		).unwrap(dal);

		pipeline_info.fragment_shader = transparent_fragment_module;
		pipeline_info.pipeline_layout = pipeline_layout;
		pipeline_info.alpha_blend = true;

		pipelines[2].first = pipeline_layout;
		pipelines[2].second = littlevk::pipeline::compile(device, pipeline_info).unwrap(dal);
	}

	{
		std::array <vk::PushConstantRange, 2> push_constant_ranges {
			vk::PushConstantRange {
				vk::ShaderStageFlagBits::eVertex,
				0, 3 * sizeof(glm::mat4)
			},
			vk::PushConstantRange {
				vk::ShaderStageFlagBits::eFragment,
				offsetof(wireframe_push_constants, color), sizeof(glm::vec3)
			},
		};

		vk::PipelineLayout pipeline_layout = littlevk::pipeline_layout(
			device,
			vk::PipelineLayoutCreateInfo {
				{}, nullptr, push_constant_ranges
			}
		).unwrap(dal);

		pipeline_info.fragment_shader = wireframe_fragment_module;
		pipeline_info.pipeline_layout = pipeline_layout;
		pipeline_info.fill_mode = vk::PolygonMode::eLine;
		pipeline_info.alpha_blend = false;

		pipelines[3].first = pipeline_layout;
		pipelines[3].second = littlevk::pipeline::compile(device, pipeline_info).unwrap(dal);
	}

	{

		std::array <vk::PushConstantRange, 2> push_constant_ranges {
			vk::PushConstantRange {
				vk::ShaderStageFlagBits::eVertex,
				0, 3 * sizeof(glm::mat4)
			},
			vk::PushConstantRange {
				vk::ShaderStageFlagBits::eFragment,
				offsetof(wireframe_push_constants, color), sizeof(glm::vec3)
			},
		};

		vk::PipelineLayout pipeline_layout = littlevk::pipeline_layout(
			device,
			vk::PipelineLayoutCreateInfo {
				{}, nullptr, push_constant_ranges
			}
		).unwrap(dal);

		pipeline_info.vertex_shader = face_vertex_module;
		pipeline_info.fragment_shader = point_color_fragment_module;
		pipeline_info.pipeline_layout = pipeline_layout;
		pipeline_info.vertex_binding = perface_vertex_binding;
		pipeline_info.vertex_attributes = perface_vertex_attributes;
		pipeline_info.fill_mode = vk::PolygonMode::eFill;

		pipelines[4].first = pipeline_layout;
		pipelines[4].second = littlevk::pipeline::compile(device, pipeline_info).unwrap(dal);
	}

	// Create the syncronization objects
	sync = littlevk::present_syncronization(device, 2).unwrap(dal);

	// Configure ImGui
	ImGui::CreateContext();
	ImGui_ImplGlfw_InitForVulkan(window->handle, true);

	// Allow popups
	ImGui::GetIO().ConfigFlags |= ImGuiWindowFlags_Popup;

	std::array <vk::DescriptorPoolSize, 4> imgui_pool_sizes {
		vk::DescriptorPoolSize {
			vk::DescriptorType::eSampler,
			1000
		},
		vk::DescriptorPoolSize {
			vk::DescriptorType::eCombinedImageSampler,
			1000
		},
		vk::DescriptorPoolSize {
			vk::DescriptorType::eSampledImage,
			1000
		},
		vk::DescriptorPoolSize {
			vk::DescriptorType::eUniformBuffer,
			1000
		},
	};

	imgui_pool = littlevk::descriptor_pool(
		device,
		vk::DescriptorPoolCreateInfo {
			{
				vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
			},
			1000, imgui_pool_sizes
		}
	).unwrap(dal);

	ImGui_ImplVulkan_InitInfo init_info {};
	init_info.Instance = littlevk::detail::get_vulkan_instance();
	init_info.PhysicalDevice = phdev;
	init_info.Device = device;
	init_info.QueueFamily = littlevk::find_graphics_queue_family(phdev);
	init_info.Queue = graphics_queue;
	init_info.DescriptorPool = imgui_pool;
	init_info.MinImageCount = 2;
	init_info.ImageCount = 2;

	ImGui_ImplVulkan_Init(&init_info, render_pass);

	// Create font atlas
	littlevk::submit_now(device, command_pool, graphics_queue,
		[&](const vk::CommandBuffer &cmd) {
			ImGui_ImplVulkan_CreateFontsTexture(cmd);
		}
	);

	ImGui_ImplVulkan_DestroyFontUploadObjects();
}

void Viewer::add(const std::string &name, const Mesh &mesh, Mode mode)
{
	MeshResource resource;

	// Mesh local = mesh;
	// recompute_normals(local);

	// Interleave the vertex data
	// assert(local.vertices.size() == local.normals.size());

	std::vector <glm::vec3> vertices;
	vertices.reserve(2 * mesh.vertices.size());

	for (size_t i = 0; i < mesh.vertices.size(); i++) {
		vertices.push_back(mesh.vertices[i]);
		vertices.push_back(mesh.normals[i]);
	}

	std::vector <glm::vec3> unindexed_vertices;
	unindexed_vertices.reserve(3 * mesh.triangles.size());

	for (const auto &triangle : mesh.triangles) {
		for (uint32_t i = 0; i < 3; i++) {
			unindexed_vertices.push_back(mesh.vertices[triangle[i]]);
			unindexed_vertices.push_back(mesh.normals[triangle[i]]);
			unindexed_vertices.push_back({ 1.0f, 0.5f, 0.5f });
		}
	}

	resource.vertex_buffer = littlevk::buffer(device,
		vertices,
		vk::BufferUsageFlagBits::eVertexBuffer,
		mem_props
	).unwrap(dal);

	resource.index_buffer = littlevk::buffer(device,
		mesh.triangles,
		vk::BufferUsageFlagBits::eIndexBuffer,
		mem_props
	).unwrap(dal);

	// TODO: defer this to a facecolors object
	resource.unindexed_vertex_buffer = littlevk::buffer(device,
		unindexed_vertices,
		vk::BufferUsageFlagBits::eVertexBuffer,
		mem_props
	).unwrap(dal);

	resource.index_count = mesh.triangles.size() * 3;
	resource.mode = mode;
	
	resource.device = device;

	meshes[name] = resource;
}

void Viewer::refresh(const std::string &name, const Mesh &mesh)
{
	auto it = meshes.find(name);
	if (it == meshes.end())
		return;

	// Mesh local = mesh;
	// recompute_normals(local);

	// Interleave the vertex data
	std::vector <glm::vec3> vertices;
	vertices.reserve(2 * mesh.vertices.size());

	for (size_t i = 0; i < mesh.vertices.size(); i++) {
		vertices.push_back(mesh.vertices[i]);
		vertices.push_back(mesh.normals[i]);
	}

	littlevk::upload(device, it->second.vertex_buffer, vertices);
}

void Viewer::replace(const std::string &name, const Mesh &mesh)
{
	auto it = meshes.find(name);
	if (it == meshes.end())
		return;

	Mesh local = mesh;
	recompute_normals(local);

	// Interleave the vertex data
	std::vector <glm::vec3> vertices;
	for (size_t i = 0; i < local.vertices.size(); i++) {
		vertices.push_back(local.vertices[i]);
		vertices.push_back(local.normals[i]);
	}

	it->second.vertex_buffer = littlevk::buffer(device,
		vertices,
		vk::BufferUsageFlagBits::eVertexBuffer,
		mem_props
	).unwrap(dal);

	it->second.index_buffer = littlevk::buffer(device,
		mesh.triangles,
		vk::BufferUsageFlagBits::eIndexBuffer,
		mem_props
	).unwrap(dal);

	it->second.index_count = mesh.triangles.size() * 3;
}

Viewer::MeshResource *Viewer::ref(const std::string &name)
{
	auto it = meshes.find(name);
	if (it == meshes.end())
		return nullptr;

	return (MeshResource *) &it->second;
}

void Viewer::clear()
{
	meshes.clear();
}

void Viewer::render()
{
	// First handle inputs
	glm::vec3 delta { 0.0f };
	if (glfwGetKey(window->handle, GLFW_KEY_W) == GLFW_PRESS)
		delta += glm::vec3 { 0.0f, 0.0f, +1.0f };
	if (glfwGetKey(window->handle, GLFW_KEY_S) == GLFW_PRESS)
		delta += glm::vec3 { 0.0f, 0.0f, -1.0f };

	if (glfwGetKey(window->handle, GLFW_KEY_A) == GLFW_PRESS)
		delta += glm::vec3 { +1.0f, 0.0f, 0.0f };
	if (glfwGetKey(window->handle, GLFW_KEY_D) == GLFW_PRESS)
		delta += glm::vec3 { -1.0f, 0.0f, 0.0f };

	if (glfwGetKey(window->handle, GLFW_KEY_Q) == GLFW_PRESS)
		delta += glm::vec3 { 0.0f, -1.0f, 0.0f };
	if (glfwGetKey(window->handle, GLFW_KEY_E) == GLFW_PRESS)
		delta += glm::vec3 { 0.0f, +1.0f, 0.0f };

	camera.move(delta * 0.1f);

	littlevk::SurfaceOperation op;
	op = littlevk::acquire_image(device, swapchain.swapchain, sync, frame);

	// Start empty render pass
	std::array <vk::ClearValue, 2> clear_values {
		vk::ClearColorValue { std::array <float, 4> { 0.0f, 0.0f, 0.0f, 1.0f } },
		vk::ClearDepthStencilValue { 1.0f, 0 }
	};

	vk::RenderPassBeginInfo render_pass_info {
		render_pass, framebuffers[op.index],
		vk::Rect2D { {}, window->extent },
		clear_values
	};

	// Record command buffer
	vk::CommandBuffer &cmd = command_buffers[frame];

	cmd.begin(vk::CommandBufferBeginInfo {});
	cmd.beginRenderPass(render_pass_info, vk::SubpassContents::eInline);

	// Configure the push constants
	push_constants constants;

	constants.proj = camera.proj(window->extent);
	constants.view = camera.view();
	constants.model = glm::mat4(1.0f);

	// Draw main interface
	for (const auto &[name, res] : meshes) {
		if (!res.enabled)
			continue;

		uint32_t mode = (uint32_t) res.mode;
		if (res.mode == Mode::Wireframe) {
			wireframe_push_constants wf_constants;
			wf_constants.proj = constants.proj;
			wf_constants.view = constants.view;
			wf_constants.model = constants.model;
			wf_constants.color = res.color;

			cmd.pushConstants(
				pipelines[mode].first,
				vk::ShaderStageFlagBits::eVertex,
				0, 3 * sizeof(glm::mat4), &wf_constants);

			cmd.pushConstants(
				pipelines[mode].first,
				vk::ShaderStageFlagBits::eFragment,
				offsetof(wireframe_push_constants, color), sizeof(glm::vec3), &wf_constants.color);
		} else {
			cmd.pushConstants <push_constants> (
				pipelines[mode].first,
				vk::ShaderStageFlagBits::eVertex,
				0, constants);
		}

		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines[mode].second);
		if (res.mode == Mode::FaceColor) {
			cmd.bindVertexBuffers(0, *res.unindexed_vertex_buffer, { 0 });
			cmd.draw(res.index_count, 1, 0, 0);
		} else {
			cmd.bindVertexBuffers(0, *res.vertex_buffer, { 0 });
			cmd.bindIndexBuffer(*res.index_buffer, 0, vk::IndexType::eUint32);
			cmd.drawIndexed(res.index_count, 1, 0, 0, 0);
		}
	}

	// Draw ImGui
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	static constexpr const char *mode_names[] = {
		// TODO: triangles with false positive coloring
		"Shaded", "Normal", "Transparent", "Wireframe", "FaceColor"
	};

	ImGui::Begin("Meshes");
	for (auto &[name, res] : meshes) {
		ImGui::Checkbox(name.c_str(), &res.enabled);

		// Select mode button for now
		for (uint32_t i = 0; i < (uint32_t) Mode::Count; i++) {
			ImGui::SameLine();

			std::string bstr = mode_names[i] + ("##" + name);
			if (ImGui::Button(bstr.c_str()))
				res.mode = (Mode) i;
		}
	}

	ImGui::End();

	ImGui::Render();
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

	cmd.endRenderPass();
	cmd.end();

	// Submit command buffer while signaling the semaphore
	vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

	vk::SubmitInfo submit_info {
		1, &sync.image_available[frame],
		&wait_stage,
		1, &cmd,
		1, &sync.render_finished[frame]
	};

	graphics_queue.submit(submit_info, sync.in_flight[frame]);

	op = littlevk::present_image(present_queue, swapchain.swapchain, sync, op.index);
	frame = 1 - frame;
}

bool Viewer::destroy()
{
	device.waitIdle();

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	delete dal;
	return littlevk::Skeleton::destroy();
}
