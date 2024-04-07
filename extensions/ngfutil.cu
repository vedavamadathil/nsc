#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdio.h>
#include <unordered_set>
#include <vector>
#include <queue>

#include "common.hpp"

__global__
void remapper_kernel(const int32_t *__restrict__ map, glm::ivec3 *__restrict__ triangles, size_t size)
{
	size_t tid = threadIdx.x + blockIdx.x * blockDim.x;
	size_t stride = blockDim.x * gridDim.x;

	for (size_t i = tid; i < size; i += stride) {
		triangles[i].x = map[triangles[i].x];
		triangles[i].y = map[triangles[i].y];
		triangles[i].z = map[triangles[i].z];
	}
}

__global__
void scatter_kernel(const int32_t *__restrict__ map, const glm::vec3 *__restrict__ data, glm::vec3 *__restrict__ dst, size_t size)
{
	size_t tid = threadIdx.x + blockIdx.x * blockDim.x;
	size_t stride = blockDim.x * gridDim.x;

	for (size_t i = tid; i < size; i += stride) {
		int32_t index = map[i];
		dst[i] = data[index];
	}
}

struct remapper : std::unordered_map <int32_t, int32_t> {
	// CUDA map
	int32_t *dev_map = nullptr; // index -> value

	explicit remapper(const std::unordered_map <int32_t, int32_t> &map)
			: std::unordered_map <int32_t, int32_t> (map) {
		// Make sure that all values are present
		// i.e. from 1 to map size
		for (int32_t i = 0; i < map.size(); i++)
			assert(this->find(i) != this->end());

		// Allocate a device map
		std::vector <int32_t> host_map(map.size());
		for (auto &kv : map)
			host_map[kv.first] = kv.second;

		cudaMalloc(&dev_map, map.size() * sizeof(int32_t));
		cudaMemcpy(dev_map, host_map.data(), map.size() * sizeof(int32_t), cudaMemcpyHostToDevice);
	}

	torch::Tensor remap(const torch::Tensor &indices) const {
		assert(indices.dtype() == torch::kInt32);
		assert(indices.is_cpu());

		torch::Tensor out = torch::zeros_like(indices);
		int32_t *out_ptr = out.data_ptr <int32_t> ();
		int32_t *indices_ptr = indices.data_ptr <int32_t> ();

		for (int32_t i = 0; i < indices.numel(); i++) {
			auto it = this->find(indices_ptr[i]);
			assert(it != this->end());
			out_ptr[i] = it->second;
		}

		return out;
	}

	torch::Tensor remap_device(const torch::Tensor &indices) const {
		assert(indices.dtype() == torch::kInt32);
		assert(indices.dim() == 2 && indices.size(1) == 3);
		assert(indices.is_cuda());

		torch::Tensor out = indices.clone();
		glm::ivec3 *out_ptr = (glm::ivec3 *) out.data_ptr <int32_t> ();

		dim3 block(256);
		dim3 grid((indices.size(0) + block.x - 1) / block.x);

		remapper_kernel <<< grid, block >>> (dev_map, out_ptr, indices.size(0));

		cudaDeviceSynchronize();
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess) {
			std::cerr << "CUDA error: " << cudaGetErrorString(err) << std::endl;
			exit(1);
		}

		return out;
	}

	torch::Tensor scatter(const torch::Tensor &vertices) const {
		assert(vertices.dtype() == torch::kFloat32);
		assert(vertices.dim() == 2 && vertices.size(1) == 3);
		assert(vertices.is_cpu());

		torch::Tensor out = torch::zeros_like(vertices);
		glm::vec3 *out_ptr = (glm::vec3 *) out.data_ptr <float> ();
		glm::vec3 *vertices_ptr = (glm::vec3 *) vertices.data_ptr <float> ();

		for (int32_t i = 0; i < vertices.size(0); i++) {
			auto it = this->find(i);
			assert(it != this->end());
			out_ptr[i] = vertices_ptr[it->second];
		}

		return out;
	}

	torch::Tensor scatter_device(const torch::Tensor &vertices) const {
		assert(vertices.dtype() == torch::kFloat32);
		assert(vertices.dim() == 2 && vertices.size(1) == 3);
		assert(vertices.is_cuda());

		torch::Tensor out = torch::zeros_like(vertices);
		glm::vec3 *out_ptr = (glm::vec3 *) out.data_ptr <float> ();
		glm::vec3 *vertices_ptr = (glm::vec3 *) vertices.data_ptr <float> ();

		dim3 block(256);
		dim3 grid((vertices.size(0) + block.x - 1) / block.x);

		scatter_kernel <<< grid, block >>> (dev_map, vertices_ptr, out_ptr, vertices.size(0));

		cudaDeviceSynchronize();
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess) {
			std::cerr << "CUDA error: " << cudaGetErrorString(err) << std::endl;
			exit(1);
		}

		return out;
	}
};

remapper generate_remapper(const torch::Tensor &complexes,
		std::unordered_map <int32_t, std::set <int32_t>> &cmap,
		int64_t vertex_count,
		int64_t sample_rate)
{
	assert(complexes.is_cpu());
	assert(complexes.dtype() == torch::kInt32);
	assert(complexes.dim() == 2 && complexes.size(1) == 4);

	std::vector <glm::ivec4> cs(complexes.size(0));
	// printf("cs: %lu\n", cs.size());
	int32_t *ptr = complexes.data_ptr <int32_t> ();
	std::memcpy(cs.data(), ptr, complexes.size(0) * sizeof(glm::ivec4));

	// Mappings
	std::unordered_map <int32_t, int32_t> rcmap;
	for (const auto &[k, v] : cmap) {
		for (const auto &i : v)
			rcmap[i] = k;
	}

	std::unordered_map <int32_t, int32_t> remap;
	// remapper remap;
	for (size_t i = 0; i < vertex_count; i++)
		remap[i] = i;

	for (const auto &[_, s] : cmap) {
		int32_t new_vertex = *s.begin();
		for (const auto &v : s)
			remap[v] = new_vertex;
	}

	std::unordered_map <ordered_pair, std::set <std::pair <int32_t, std::vector <int32_t>>>, ordered_pair::hash> bmap;

	for (int32_t i = 0; i < cs.size(); i++) {
		int32_t i00 = i * sample_rate * sample_rate;
		int32_t i10 = i00 + (sample_rate - 1);
		int32_t i01 = i00 + (sample_rate - 1) * sample_rate;
		int32_t i11 = i00 + (sample_rate * sample_rate - 1);

		int32_t c00 = rcmap[i00];
		int32_t c10 = rcmap[i10];
		int32_t c01 = rcmap[i01];
		int32_t c11 = rcmap[i11];

		ordered_pair p;
		bool reversed;

		std::vector <int32_t> b00_10;
		std::vector <int32_t> b00_01;
		std::vector <int32_t> b10_11;
		std::vector <int32_t> b01_11;

		// 00 -> 10
		reversed = p.from(c00, c10);
		if (reversed) {
			for (int32_t i = sample_rate - 2; i >= 1; i--)
				b00_10.push_back(i + i00);
		} else {
			for (int32_t i = 1; i <= sample_rate - 2; i++)
				b00_10.push_back(i + i00);
		}

		bmap[p].insert({ i, b00_10 });

		// 00 -> 01
		reversed = p.from(c00, c01);
		if (reversed) {
			for (int32_t i = sample_rate * (sample_rate - 2); i >= sample_rate; i -= sample_rate)
				b00_01.push_back(i + i00);
		} else {
			for (int32_t i = sample_rate; i <= sample_rate * (sample_rate - 2); i += sample_rate)
				b00_01.push_back(i + i00);
		}

		bmap[p].insert({ i, b00_01 });

		// 10 -> 11
		reversed = p.from(c10, c11);
		if (reversed) {
			for (int32_t i = sample_rate - 2; i >= 1; i--)
				b10_11.push_back(i * sample_rate + sample_rate - 1 + i00);
		} else {
			for (int32_t i = 1; i <= sample_rate - 2; i++)
				b10_11.push_back(i * sample_rate + sample_rate - 1 + i00);
		}

		bmap[p].insert({ i, b10_11 });

		// 01 -> 11
		reversed = p.from(c01, c11);
		if (reversed) {
			for (int32_t i = sample_rate - 2; i >= 1; i--)
				b01_11.push_back((sample_rate - 1) * sample_rate + i + i00);
		} else {
			for (int32_t i = 1; i <= sample_rate - 2; i++)
				b01_11.push_back((sample_rate - 1) * sample_rate + i + i00);
		}

		bmap[p].insert({ i, b01_11 });
	}

	for (const auto &[p, bs] : bmap) {
		const auto &ref = *bs.begin();
		for (const auto &b : bs) {
			for (int32_t i = 0; i < b.second.size(); i++) {
				remap[b.second[i]] = ref.second[i];
			}
		}
	}

	return remapper(remap);
}

std::tuple <torch::Tensor, torch::Tensor> deduplicate(const torch::Tensor &vertices, const torch::Tensor &triangles)
{
	assert(vertices.is_cpu());
	assert(vertices.dtype() == torch::kFloat32);
	assert(vertices.dim() == 2 && vertices.size(1) == 3);

	assert(triangles.is_cpu());
	assert(triangles.dtype() == torch::kInt32);
	assert(triangles.dim() == 2 && triangles.size(1) == 3);

	const float *vertices_raw = vertices.data_ptr <float> ();
	const int32_t *indices_raw = triangles.data_ptr <int32_t> ();

	size_t vertex_count = vertices.size(0);
	size_t index_count = triangles.numel();

	glm::vec3 *vertices_ptr = (glm::vec3 *) vertices_raw;

	std::unordered_map <glm::vec3, int32_t> hashed;

	std::vector <glm::vec3> new_vertices;
	std::vector <int32_t> new_indices;

	for (size_t i = 0; i < index_count; i++) {
		int32_t vi = indices_raw[i];

		glm::vec3 vertex = vertices_ptr[vi];
		if (!hashed.count(vertex)) {
			int32_t ni = new_vertices.size();
			new_vertices.push_back(vertex);
			hashed[vertex] = ni;

		}

		new_indices.push_back(hashed[vertex]);
	}

	auto options = torch::TensorOptions()
		.dtype(torch::kFloat32)
		.device(torch::kCPU, 0);

	torch::Tensor tch_new_vertices = torch::zeros({ (long) new_vertices.size(), 3 }, options);
	torch::Tensor tch_new_triangles = torch::zeros_like(triangles);

	float *new_vertices_raw = tch_new_vertices.data_ptr <float> ();
	int32_t *new_indices_raw = tch_new_triangles.data_ptr <int32_t> ();

	std::memcpy(new_vertices_raw, new_vertices.data(), sizeof(glm::vec3) * new_vertices.size());
	std::memcpy(new_indices_raw, new_indices.data(), sizeof(int32_t) * new_indices.size());

	return { tch_new_vertices, tch_new_triangles };
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
        py::class_ <geometry> (m, "geometry")
                .def(py::init <const torch::Tensor &, const torch::Tensor &> ())
                .def(py::init <const torch::Tensor &, const torch::Tensor &, const torch::Tensor &> ())
		.def("deduplicate", &geometry::deduplicate)
		.def("torched", &geometry::torched)
		.def_readonly("vertices", &geometry::vertices)
		.def_readonly("normals", &geometry::normals)
		.def_readonly("triangles", &geometry::triangles)
		.def("__repr__", [](const geometry &g) {
			return "geometry(vertices=" + std::to_string(g.vertices.size())
				+ ", triangles=" + std::to_string(g.triangles.size()) + ")";
		});

	py::class_ <Graph> (m, "Graph")
		.def(py::init <const torch::Tensor &, size_t> ())
		.def("smooth", &Graph::smooth);

	py::class_ <remapper> (m, "remapper")
		.def("remap", &remapper::remap, "Remap indices")
		.def("remap_device", &remapper::remap_device, "Remap indices")
		.def("scatter", &remapper::scatter, "Scatter vertex data")
		.def("scatter_device", &remapper::scatter_device, "Scatter vertex data");

	m.def("cluster_geometry", &cluster_geometry);
	m.def("triangulate_shorted", &triangulate_shorted);
	m.def("generate_remapper", &generate_remapper, "Generate remapper");
	m.def("mesh_deduplicate", &deduplicate, "Deduplicate mesh vertices and reindex the mesh");
	m.def("parametrize_chart", &parametrize, "Parametrize a chart with disk topology");
	m.def("parametrize_multicharts", &parametrize_parallel, "Parametrize multiple charts with disk topology in parallel");
	m.def("load_mesh", &load_mesh);
}
