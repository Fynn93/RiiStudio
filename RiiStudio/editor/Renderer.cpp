#include "Renderer.hpp"

#include <GL/gl3w.h>
#include "GLFW/glfw3.h"

#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

#include <imgui/imgui.h>
#include <array>

#include <LibCube/GX/Shader/GXMaterial.hpp>

#include "renderer/ShaderProgram.hpp"
#include "renderer/ShaderCache.hpp"
#include "renderer/UBOBuilder.hpp"

struct SceneTree
{
	struct Node
	{
		const lib3d::Material& mat;
		const lib3d::Polygon& poly;
		const lib3d::Bone& bone;
		u8 priority;

		ShaderProgram& shader;

		// Only used later
		mutable u32 idx_ofs = 0;
		mutable u32 idx_size = 0;
		mutable u32 mtx_id = 0;

		bool isTranslucent() const
		{
			return mat.isXluPass();
		}

	};


	std::vector<Node> opaque, translucent;


	void gatherBoneRecursive(u64 boneId, const px::CollectionHost::CollectionHandle& bones,
		const px::CollectionHost::CollectionHandle& mats, const px::CollectionHost::CollectionHandle& polys)
	{
		const auto pBone = bones.at<lib3d::Bone>(boneId);
		if (!pBone) return;

		const u64 nDisplay = pBone->getNumDisplays();
		for (u64 i = 0; i < nDisplay; ++i)
		{
			const auto display = pBone->getDisplay(i);
			const auto shader_sources = mats.at<lib3d::Material>(display.matId)->generateShaders();
			ShaderProgram& shader = ShaderCache::compile(shader_sources.first, shader_sources.second);
			const Node node{ *mats.at<lib3d::Material>(display.matId),
				*polys.at<lib3d::Polygon>(display.polyId), *pBone, display.prio, shader };

			if (node.isTranslucent())
				translucent.push_back(node);
			else
				opaque.push_back(node);
		}

		for (u64 i = 0; i < pBone->getNumChildren(); ++i)
			gatherBoneRecursive(pBone->getChild(i), bones, mats, polys);
	}

	void gather(const px::CollectionHost& root)
	{
		const auto pMats = root.getFolder<lib3d::Material>();
		if (!pMats.has_value())
			return;
		const auto pPolys = root.getFolder<lib3d::Polygon>();
		if (!pPolys.has_value())
			return;

		// We cannot proceed without bones, as we attach all render instructions to them.
		const auto pBones = root.getFolder<lib3d::Bone>();
		if (!pBones.has_value())
			return;

		// Assumes root at zero
		if (pBones->size() == 0)
			return;
		gatherBoneRecursive(0, pBones.value(), pMats.value(), pPolys.value());
	}

	// TODO: Z Sort
};


struct VBOBuilder
{
	VBOBuilder()
	{
		glGenBuffers(1, &mPositionBuf);
		glGenBuffers(1, &mIndexBuf);

		mAttribStack.push_back(0);
	}
	~VBOBuilder()
	{
		glDeleteBuffers(1, &mPositionBuf);
		glDeleteBuffers(1, &mIndexBuf);
	}

	std::vector<u8> mData;
	std::vector<u32> mIndices;

	void next() { mAttribStack.push_back(mData.size()); }

	template<typename T>
	void push(const T& data)
	{
		const std::size_t begin = mData.size();
		mData.resize(mData.size() + sizeof(T));
		*reinterpret_cast<T*>(mData.data() + begin) = data;
	}

	void build()
	{
		std::vector<int> mIds{ 0, 5, 7 }; // Hack

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIndexBuf);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, mIndices.size() * 4, mIndices.data(), GL_STATIC_DRAW);

		glBindBuffer(GL_ARRAY_BUFFER, mPositionBuf);

		glBufferData(GL_ARRAY_BUFFER, mData.size(), mData.data(), GL_STATIC_DRAW);
		for (std::size_t i = 0; i < mAttribStack.size(); ++i)
			glVertexAttribPointer(mIds[i], 3, GL_FLOAT, GL_FALSE, 3 * 4, (void*)mAttribStack[i]);
	}
	u32 mPositionBuf, mIndexBuf;

	std::vector<u32> mAttribStack;
};

struct SceneState
{
	std::vector<glm::vec3> mPositions;
	std::vector<glm::vec3> mColors;
	std::vector<glm::vec2> mUvs;


	std::vector<u32> mIndices;
	DelegatedUBOBuilder mUboBuilder;
	SceneTree mTree;

	std::vector<lib3d::Polygon::SimpleVertex> vtxScratch;

	
	void buildBuffers()
	{
		mPositions.clear();
		mColors.clear();
		mUvs.clear();
		mIndices.clear();

		for (const auto& node : mTree.opaque)
		{
			vtxScratch.clear();
			node.poly.propogate(vtxScratch);
			node.idx_ofs = mIndices.size();
			u32 i = mIndices.size();
			for (const auto& v : vtxScratch)
			{
				mPositions.push_back(v.position);
				mColors.push_back(v.colors[0]);
				mUvs.push_back(glm::vec3(v.uvs[0].x, v.uvs[1].y, 0));
				mIndices.push_back(i);
				++i;
			}
			node.idx_size = vtxScratch.size();
		}

		for (const auto& pos : mPositions)
			mVbo.push(pos);
		mVbo.next();
		for (const auto& clr : mColors)
			mVbo.push(clr);
		mVbo.next();
		for (const auto& uv : mUvs)
			mVbo.push(uv);

		mVbo.mIndices = mIndices;

		glBindVertexArray(VAO);
		glEnableVertexAttribArray(0); // pos
		glEnableVertexAttribArray(5); // clr
		glEnableVertexAttribArray(7); // uv0
		
		mVbo.build();
		glBindVertexArray(0);

	}
	void gather(const px::CollectionHost& root)
	{
		mTree.gather(root);
		buildBuffers();

		//	const auto mats = root.getFolder<lib3d::Material>();
		//	const auto mat = mats->at<lib3d::Material>(0);
		//	const auto compiled = mat->generateShaders();
	}
	void build(const glm::mat4& view, const glm::mat4& proj)
	{
		mUboBuilder.clear();
		u32 i = 0;
		for (const auto& node : mTree.opaque)
		{
			lib3d::SRT3 srt = node.bone.getSRT();
			glm::mat4 mdl(1.0f);

			mdl = glm::scale(mdl, srt.scale);
			// mdl = glm::rotate(mdl, (glm::mat4)srt.rotation);
			mdl = glm::translate(mdl, srt.translation);

			node.mat.generateUniforms(mUboBuilder, mdl, view, proj);
			node.mtx_id = i;
			++i;
		}


		mUboBuilder.submit();
	}

	void draw()
	{
		glClearColor(0.2f, 0.3f, 0.3f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);

		glBindVertexArray(VAO);

		for (const auto& node : mTree.opaque)
		{
		
			glUseProgram(node.shader.getId());
			mUboBuilder.use(node.mtx_id);
			glDrawElements(GL_TRIANGLES, node.idx_size, GL_UNSIGNED_INT, (void*)(node.idx_ofs * 4));
		}
		glBindVertexArray(0);
	}

	SceneState()
	{	
		glGenVertexArrays(1, &VAO);

		vtxScratch.resize(250);
	}
	~SceneState()
	{
		glDeleteVertexArrays(1, &VAO);
	}

	u32 VAO;

	VBOBuilder mVbo;
};

void Renderer::prepare(const px::CollectionHost& root)
{
	mState->gather(root);
}

static void cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, GLvoid* userParam)
{
	printf(message);
	printf("\n");
}
Renderer::Renderer()
{

	glDebugMessageCallback(cb, 0);

	mState = std::make_unique<SceneState>();
}
Renderer::~Renderer() {}
void Renderer::render(u32 width, u32 height)
{
	if (!bShadersLoaded)
		createShaderProgram();
	ImGui::DragFloat("EyeX", &eye.x, .01f, -10, 30);
	ImGui::DragFloat("EyeY", &eye.y, .01f, -10, 30);
	ImGui::DragFloat("EyeZ", &eye.z, .01f, -10, 30);

	static bool rend = false;
	ImGui::Checkbox("Render", &rend);
	if (!rend) return;

	static bool wireframe = false;
	ImGui::Checkbox("Wireframe", &wireframe);
	if (wireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	else
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	// horizontal angle : toward -Z
	float horizontalAngle = 3.14f;
	// vertical angle : 0, look at the horizon
	float verticalAngle = 0.0f;
	// Initial Field of View
	float initialFoV = 45.0f;

	float speed = 3.0f; // 3 units / second
	float mouseSpeed = 0.005f;

	float deltaTime = .1f;

	const auto [xpos, ypos] = ImGui::GetMousePos();

	horizontalAngle += mouseSpeed * deltaTime * float(width - xpos);
	verticalAngle += mouseSpeed * deltaTime * float(height - ypos);
	// Direction : Spherical coordinates to Cartesian coordinates conversion
	glm::vec3 direction(
		cos(verticalAngle) * sin(horizontalAngle),
		sin(verticalAngle),
		cos(verticalAngle) * cos(horizontalAngle)
	);
	glm::vec3 right = glm::vec3(
		sin(horizontalAngle - 3.14f / 2.0f),
		0,
		cos(horizontalAngle - 3.14f / 2.0f)
	);
	glm::vec3 up = glm::cross(right, direction);

	if (ImGui::IsKeyDown('W'))
		eye += direction * deltaTime * speed;
	if (ImGui::IsKeyDown('S'))
		eye -= direction * deltaTime * speed;
	if (ImGui::IsKeyDown('A'))
		eye -= right * deltaTime * speed;
	if (ImGui::IsKeyDown('D'))
		eye += right * deltaTime * speed;

	glm::mat4 projMtx = glm::perspective(glm::radians(45.0f), static_cast<f32>(width) / static_cast<f32>(height), 0.1f, 100.f);
	glm::mat4 viewMtx = glm::lookAt(
		eye,
		eye + direction,
		up
	);

	mState->build(projMtx, viewMtx);
	mState->draw();
}