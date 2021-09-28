#include <stdlib.h>     // For seeding random and rand()
#include <time.h>       // For grabbing time (to seed random)

#include "Game.h"
#include "Vertex.h"
#include "Input.h"

#include "ImGUI/imgui.h"
#include "WICTextureLoader.h"

// Needed for a helper function to read compiled shader files from the hard drive
#pragma comment(lib, "d3dcompiler.lib")
#include <d3dcompiler.h>
#include "ImGUI/imgui_impl_win32.h"
#include "ImGUI/imgui_impl_dx11.h"

// For the DirectX Math library
using namespace DirectX;

// Helper macro for getting a float between min and max
#define RandomRange(min, max) (float)rand() / RAND_MAX * (max - min) + min

// Helper macros for making texture and shader loading code more succinct
#define LoadTexture(file, srv) CreateWICTextureFromFile(device.Get(), context.Get(), GetFullPathTo_Wide(file).c_str(), 0, srv.GetAddressOf())
#define LoadShader(type, file) new type(device.Get(), context.Get(), GetFullPathTo_Wide(file).c_str())

#define GET_VARIABLE_NAME(var) (#var)

// --------------------------------------------------------
// Constructor
//
// DXCore (base class) constructor will set up underlying fields.
// DirectX itself, and our window, are not ready yet!
//
// hInstance - the application's OS-level handle (unique ID)
// --------------------------------------------------------
Game::Game(HINSTANCE hInstance)
	: DXCore(
		hInstance,		   // The application's handle
		"DirectX Game",	   // Text for the window's title bar
		1280,			   // Width of the window's client area
		720,			   // Height of the window's client area
		true)			   // Show extra stats (fps) in title bar?
{
	camera = 0;

	// Seed random
	srand((unsigned int)time(0));

#if defined(DEBUG) || defined(_DEBUG)
	// Do we want a console window?  Probably only in debug mode
	CreateConsoleWindow(500, 120, 32, 120);
	printf("Console window created successfully.  Feel free to printf() here.\n");
#endif

}

// --------------------------------------------------------
// Destructor - Clean up anything our game has created:
//  - Release all DirectX objects created here
//  - Delete any objects to prevent memory leaks
// --------------------------------------------------------
Game::~Game()
{
	// Note: Since we're using smart pointers (ComPtr),
	// we don't need to explicitly clean up those DirectX objects
	// - If we weren't using smart pointers, we'd need
	//   to call Release() on each DirectX object

	// Clean up our other resources
	for (auto& m : meshes) delete m;
	for (auto& s : shaders) delete s; 
	for (auto& m : materials) delete m;
	for (auto& e : entities) delete e;

	// Delete any one-off objects
	delete sky;
	delete camera;
	delete renderer;
	delete arial;
	delete spriteBatch;

	// Delete singletons
	delete& Input::GetInstance();

	// ImGui clean up
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	// PhysX
	mPhysics->release();
	//mScene->release();
	mDispatcher->release();
	mFoundation->release();
}

// --------------------------------------------------------
// Called once per program, after DirectX and the window
// are initialized but before the game loop.
// --------------------------------------------------------
void Game::Init()
{
	// Initialize the input manager with the window's handle
	Input::GetInstance().Initialize(this->hWnd);

	// Asset loading and entity creation
	LoadAssetsAndCreateEntities();
	
	// Tell the input assembler stage of the pipeline what kind of
	// geometric primitives (points, lines or triangles) we want to draw.  
	// Essentially: "What kind of shape should the GPU draw with our data?"
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Set up lights initially
	lightCount = 64;
	GenerateLights();

	// Make our camera
	camera = new Camera(
		0, 0, -10,	// Position
		3.0f,		// Move speed
		1.0f,		// Mouse look
		this->width / (float)this->height); // Aspect ratio

	interval = 0.005;

	// Initialize ImGui
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	
	// Pick a style (uncomment one of these 3)
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsLight();
	//ImGui::StyleColorsClassic();

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX11_Init(device.Get(), context.Get());

	// PhysX
	mFoundation = PxCreateFoundation(PX_PHYSICS_VERSION, mDefaultAllocatorCallback, mDefaultErrorCallback);
	if (!mFoundation) throw("PxCreateFoundation failed!");
	mToleranceScale.length = 100;        // typical length of an object
	mToleranceScale.speed = 981;         // typical speed of an object, gravity*1s is a reasonable choice
	mPhysics = PxCreatePhysics(PX_PHYSICS_VERSION, *mFoundation, mToleranceScale, true, NULL);

	physx::PxSceneDesc sceneDesc(mPhysics->getTolerancesScale());
	sceneDesc.gravity = physx::PxVec3(0.0f, -9.81f, 0.0f);
	mDispatcher = physx::PxDefaultCpuDispatcherCreate(2);
	sceneDesc.cpuDispatcher = mDispatcher;
	sceneDesc.filterShader = physx::PxDefaultSimulationFilterShader;
	mScene = mPhysics->createScene(sceneDesc);



	mMaterial = mPhysics->createMaterial(0.5f, 0.5f, 0.6f);
	physx::PxRigidStatic* groundPlane = PxCreatePlane(*mPhysics, physx::PxPlane(0, 1, 0, 50), *mMaterial);
	mScene->addActor(*groundPlane);

	float halfExtent = .5f;
	physx::PxShape* shape = mPhysics->createShape(physx::PxBoxGeometry(halfExtent, halfExtent, halfExtent), *mMaterial);
	physx::PxU32 size = 10;
	physx::PxTransform t(physx::PxVec3(0));
	for (physx::PxU32 i = 0; i < size; i++) {
		for (physx::PxU32 j = 0; j < size - i; j++) {
			physx::PxTransform localTm(physx::PxVec3(physx::PxReal(j * 2) - physx::PxReal(size - i), physx::PxReal(i * 2 + 1), 0) * halfExtent);
			physx::PxRigidDynamic* body = mPhysics->createRigidDynamic(t.transform(localTm));
			body->attachShape(*shape);
			physx::PxRigidBodyExt::updateMassAndInertia(*body, 10.0f);
			mScene->addActor(*body);
		}
	}
	shape->release();
}


// --------------------------------------------------------
// Load all assets and create materials, entities, etc.
// --------------------------------------------------------
void Game::LoadAssetsAndCreateEntities()
{
	// Load shaders using our succinct LoadShader() macro
	SimpleVertexShader* vertexShader	= LoadShader(SimpleVertexShader, L"VertexShader.cso");
	pixelShader							= LoadShader(SimplePixelShader, L"PixelShader.cso");
	pixelShaderPBR						= LoadShader(SimplePixelShader, L"PixelShaderPBR.cso");
	SimplePixelShader* solidColorPS		= LoadShader(SimplePixelShader, L"SolidColorPS.cso");
	
	SimpleVertexShader* skyVS = LoadShader(SimpleVertexShader, L"SkyVS.cso");
	SimplePixelShader* skyPS  = LoadShader(SimplePixelShader, L"SkyPS.cso");

	shaders.push_back(vertexShader);
	shaders.push_back(pixelShader);
	shaders.push_back(pixelShaderPBR);
	shaders.push_back(solidColorPS);
	shaders.push_back(skyVS);
	shaders.push_back(skyPS);

	// Set up the sprite batch and load the sprite font
	spriteBatch = new SpriteBatch(context.Get());
	arial = new SpriteFont(device.Get(), GetFullPathTo_Wide(L"../../Assets/Textures/arial.spritefont").c_str());

	// Make the meshes
	Mesh* sphereMesh = new Mesh(GetFullPathTo("../../Assets/Models/sphere.obj").c_str(), device);
	Mesh* helixMesh = new Mesh(GetFullPathTo("../../Assets/Models/helix.obj").c_str(), device);
	Mesh* cubeMesh = new Mesh(GetFullPathTo("../../Assets/Models/cube.obj").c_str(), device);
	Mesh* coneMesh = new Mesh(GetFullPathTo("../../Assets/Models/cone.obj").c_str(), device);

	meshes.push_back(sphereMesh);
	meshes.push_back(helixMesh);
	meshes.push_back(cubeMesh);
	meshes.push_back(coneMesh);

	
	// Declare the textures we'll need
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cobbleA,  cobbleN,  cobbleR,  cobbleM;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> floorA,  floorN,  floorR,  floorM;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> paintA,  paintN,  paintR,  paintM;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scratchedA,  scratchedN,  scratchedR,  scratchedM;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> bronzeA,  bronzeN,  bronzeR,  bronzeM;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> roughA,  roughN,  roughR,  roughM;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> woodA,  woodN,  woodR,  woodM;

	// Load the textures using our succinct LoadTexture() macro
	LoadTexture(L"../../Assets/Textures/cobblestone_albedo.png", cobbleA);
	LoadTexture(L"../../Assets/Textures/cobblestone_normals.png", cobbleN);
	LoadTexture(L"../../Assets/Textures/cobblestone_roughness.png", cobbleR);
	LoadTexture(L"../../Assets/Textures/cobblestone_metal.png", cobbleM);

	LoadTexture(L"../../Assets/Textures/floor_albedo.png", floorA);
	LoadTexture(L"../../Assets/Textures/floor_normals.png", floorN);
	LoadTexture(L"../../Assets/Textures/floor_roughness.png", floorR);
	LoadTexture(L"../../Assets/Textures/floor_metal.png", floorM);
	
	LoadTexture(L"../../Assets/Textures/paint_albedo.png", paintA);
	LoadTexture(L"../../Assets/Textures/paint_normals.png", paintN);
	LoadTexture(L"../../Assets/Textures/paint_roughness.png", paintR);
	LoadTexture(L"../../Assets/Textures/paint_metal.png", paintM);
	
	LoadTexture(L"../../Assets/Textures/scratched_albedo.png", scratchedA);
	LoadTexture(L"../../Assets/Textures/scratched_normals.png", scratchedN);
	LoadTexture(L"../../Assets/Textures/scratched_roughness.png", scratchedR);
	LoadTexture(L"../../Assets/Textures/scratched_metal.png", scratchedM);
	
	LoadTexture(L"../../Assets/Textures/bronze_albedo.png", bronzeA);
	LoadTexture(L"../../Assets/Textures/bronze_normals.png", bronzeN);
	LoadTexture(L"../../Assets/Textures/bronze_roughness.png", bronzeR);
	LoadTexture(L"../../Assets/Textures/bronze_metal.png", bronzeM);
	
	LoadTexture(L"../../Assets/Textures/rough_albedo.png", roughA);
	LoadTexture(L"../../Assets/Textures/rough_normals.png", roughN);
	LoadTexture(L"../../Assets/Textures/rough_roughness.png", roughR);
	LoadTexture(L"../../Assets/Textures/rough_metal.png", roughM);
	
	LoadTexture(L"../../Assets/Textures/wood_albedo.png", woodA);
	LoadTexture(L"../../Assets/Textures/wood_normals.png", woodN);
	LoadTexture(L"../../Assets/Textures/wood_roughness.png", woodR);
	LoadTexture(L"../../Assets/Textures/wood_metal.png", woodM);

	textures.push_back(cobbleA); textures.push_back(cobbleN); textures.push_back(cobbleR); textures.push_back(cobbleM);
	textures.push_back(floorA); textures.push_back(floorN); textures.push_back(floorR); textures.push_back(floorM);
	textures.push_back(paintA); textures.push_back(paintN); textures.push_back(paintR); textures.push_back(paintM);
	textures.push_back(scratchedA); textures.push_back(scratchedN); textures.push_back(scratchedR); textures.push_back(scratchedM);
	textures.push_back(bronzeA); textures.push_back(bronzeN); textures.push_back(bronzeR); textures.push_back(bronzeM);
	textures.push_back(roughA); textures.push_back(roughN); textures.push_back(roughR); textures.push_back(roughM);
	textures.push_back(woodA); textures.push_back(woodN); textures.push_back(woodR); textures.push_back(woodM);

	// Describe and create our sampler state
	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	sampDesc.MaxAnisotropy = 16;
	sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
	device->CreateSamplerState(&sampDesc, samplerOptions.GetAddressOf());


	// Create the sky using a DDS cube map
	/*sky = new Sky(
		GetFullPathTo_Wide(L"..\\..\\Assets\\Skies\\SunnyCubeMap.dds").c_str(),
		cubeMesh,
		skyVS,
		skyPS,
		samplerOptions,
		device,
		context);*/

	// Create the sky using 6 images
	sky = new Sky(
		GetFullPathTo_Wide(L"..\\..\\Assets\\Skies\\Night\\right.png").c_str(),
		GetFullPathTo_Wide(L"..\\..\\Assets\\Skies\\Night\\left.png").c_str(),
		GetFullPathTo_Wide(L"..\\..\\Assets\\Skies\\Night\\up.png").c_str(),
		GetFullPathTo_Wide(L"..\\..\\Assets\\Skies\\Night\\down.png").c_str(),
		GetFullPathTo_Wide(L"..\\..\\Assets\\Skies\\Night\\front.png").c_str(),
		GetFullPathTo_Wide(L"..\\..\\Assets\\Skies\\Night\\back.png").c_str(),
		cubeMesh,
		skyVS,
		skyPS,
		samplerOptions,
		device,
		context);

	// Create basic materials
	Material* cobbleMat2x = new Material(vertexShader, pixelShader, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), cobbleA, cobbleN, cobbleR, cobbleM, samplerOptions);
	Material* floorMat = new Material(vertexShader, pixelShader, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), floorA, floorN, floorR, floorM, samplerOptions);
	Material* paintMat = new Material(vertexShader, pixelShader, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), paintA, paintN, paintR, paintM, samplerOptions);
	Material* scratchedMat = new Material(vertexShader, pixelShader, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), scratchedA, scratchedN, scratchedR, scratchedM, samplerOptions);
	Material* bronzeMat = new Material(vertexShader, pixelShader, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), bronzeA, bronzeN, bronzeR, bronzeM, samplerOptions);
	Material* roughMat = new Material(vertexShader, pixelShader, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), roughA, roughN, roughR, roughM, samplerOptions);
	Material* woodMat = new Material(vertexShader, pixelShader, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), woodA, woodN, woodR, woodM, samplerOptions);

	materials.push_back(cobbleMat2x);
	materials.push_back(floorMat);
	materials.push_back(paintMat);
	materials.push_back(scratchedMat);
	materials.push_back(bronzeMat);
	materials.push_back(roughMat);
	materials.push_back(woodMat);

	// Create PBR materials
	Material* cobbleMat2xPBR = new Material(vertexShader, pixelShaderPBR, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), cobbleA, cobbleN, cobbleR, cobbleM, samplerOptions);
	Material* floorMatPBR = new Material(vertexShader, pixelShaderPBR, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), floorA, floorN, floorR, floorM, samplerOptions);
	Material* paintMatPBR = new Material(vertexShader, pixelShaderPBR, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), paintA, paintN, paintR, paintM, samplerOptions);
	Material* scratchedMatPBR = new Material(vertexShader, pixelShaderPBR, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), scratchedA, scratchedN, scratchedR, scratchedM, samplerOptions);
	Material* bronzeMatPBR = new Material(vertexShader, pixelShaderPBR, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), bronzeA, bronzeN, bronzeR, bronzeM, samplerOptions);
	Material* roughMatPBR = new Material(vertexShader, pixelShaderPBR, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), roughA, roughN, roughR, roughM, samplerOptions);
	Material* woodMatPBR = new Material(vertexShader, pixelShaderPBR, XMFLOAT4(1, 1, 1, 1), 256.0f, XMFLOAT2(2, 2), woodA, woodN, woodR, woodM, samplerOptions);

	materials.push_back(cobbleMat2xPBR);
	materials.push_back(floorMatPBR);
	materials.push_back(paintMatPBR);
	materials.push_back(scratchedMatPBR);
	materials.push_back(bronzeMatPBR);
	materials.push_back(roughMatPBR);
	materials.push_back(woodMatPBR);



	// === Create the PBR entities =====================================
	/*GameEntity* cobSpherePBR = new GameEntity(sphereMesh, cobbleMat2xPBR);
	cobSpherePBR->GetTransform()->SetScale(2, 2, 2);
	cobSpherePBR->GetTransform()->SetPosition(-6, 2, 0);

	GameEntity* floorSpherePBR = new GameEntity(sphereMesh, floorMatPBR);
	floorSpherePBR->GetTransform()->SetScale(2, 2, 2);
	floorSpherePBR->GetTransform()->SetPosition(-4, 2, 0);

	GameEntity* paintSpherePBR = new GameEntity(sphereMesh, paintMatPBR);
	paintSpherePBR->GetTransform()->SetScale(2, 2, 2);
	paintSpherePBR->GetTransform()->SetPosition(-2, 2, 0);

	GameEntity* scratchSpherePBR = new GameEntity(sphereMesh, scratchedMatPBR);
	scratchSpherePBR->GetTransform()->SetScale(2, 2, 2);
	scratchSpherePBR->GetTransform()->SetPosition(0, 2, 0);

	GameEntity* bronzeSpherePBR = new GameEntity(sphereMesh, bronzeMatPBR);
	bronzeSpherePBR->GetTransform()->SetScale(2, 2, 2);
	bronzeSpherePBR->GetTransform()->SetPosition(2, 2, 0);

	GameEntity* roughSpherePBR = new GameEntity(sphereMesh, roughMatPBR);
	roughSpherePBR->GetTransform()->SetScale(2, 2, 2);
	roughSpherePBR->GetTransform()->SetPosition(4, 2, 0);

	GameEntity* woodSpherePBR = new GameEntity(sphereMesh, woodMatPBR);
	woodSpherePBR->GetTransform()->SetScale(2, 2, 2);
	woodSpherePBR->GetTransform()->SetPosition(6, 2, 0);

	entities.push_back(cobSpherePBR);
	entities.push_back(floorSpherePBR);
	entities.push_back(paintSpherePBR);
	entities.push_back(scratchSpherePBR);
	entities.push_back(bronzeSpherePBR);
	entities.push_back(roughSpherePBR);
	entities.push_back(woodSpherePBR);

	// Create the non-PBR entities ==============================
	GameEntity* cobSphere = new GameEntity(sphereMesh, cobbleMat2x);
	cobSphere->GetTransform()->SetScale(2, 2, 2);
	cobSphere->GetTransform()->SetPosition(-6, -2, 0);

	GameEntity* floorSphere = new GameEntity(sphereMesh, floorMat);
	floorSphere->GetTransform()->SetScale(2, 2, 2);
	floorSphere->GetTransform()->SetPosition(-4, -2, 0);

	GameEntity* paintSphere = new GameEntity(sphereMesh, paintMat);
	paintSphere->GetTransform()->SetScale(2, 2, 2);
	paintSphere->GetTransform()->SetPosition(-2, -2, 0);

	GameEntity* scratchSphere = new GameEntity(sphereMesh, scratchedMat);
	scratchSphere->GetTransform()->SetScale(2, 2, 2);
	scratchSphere->GetTransform()->SetPosition(0, -2, 0);

	GameEntity* bronzeSphere = new GameEntity(sphereMesh, bronzeMat);
	bronzeSphere->GetTransform()->SetScale(2, 2, 2);
	bronzeSphere->GetTransform()->SetPosition(2, -2, 0);

	GameEntity* roughSphere = new GameEntity(sphereMesh, roughMat);
	roughSphere->GetTransform()->SetScale(2, 2, 2);
	roughSphere->GetTransform()->SetPosition(4, -2, 0);

	GameEntity* woodSphere = new GameEntity(sphereMesh, woodMat);
	woodSphere->GetTransform()->SetScale(2, 2, 2);
	woodSphere->GetTransform()->SetPosition(6, -2, 0);

	entities.push_back(cobSphere);
	entities.push_back(floorSphere);
	entities.push_back(paintSphere);
	entities.push_back(scratchSphere);
	entities.push_back(bronzeSphere);
	entities.push_back(roughSphere);
	entities.push_back(woodSphere);*/

	GameEntity* cobSpherePBR = new GameEntity(sphereMesh, cobbleMat2xPBR);
	cobSpherePBR->GetTransform()->SetScale(3, 3, 3);
	cobSpherePBR->GetTransform()->SetPosition(0, 0, 0);
	
	GameEntity* floorSpherePBR = new GameEntity(sphereMesh, floorMatPBR);
	floorSpherePBR->GetTransform()->SetScale(2, 2, 2);
	floorSpherePBR->GetTransform()->SetPosition(4, 0, 0);

	GameEntity* scratchSpherePBR = new GameEntity(sphereMesh, scratchedMatPBR);
	scratchSpherePBR->GetTransform()->SetScale(2, 2, 2);
	scratchSpherePBR->GetTransform()->SetPosition(-4, 0, 0);

	GameEntity* bronzeSpherePBR = new GameEntity(sphereMesh, bronzeMatPBR);
	bronzeSpherePBR->GetTransform()->SetPosition(6, 0, 0);

	GameEntity* paintSpherePBR = new GameEntity(sphereMesh, paintMatPBR);
	paintSpherePBR->GetTransform()->SetScale(0.5, 0.5, 0.5);
	paintSpherePBR->GetTransform()->SetPosition(6, 1, 0);

	cobSpherePBR->GetTransform()->AddChild(floorSpherePBR->GetTransform());
	cobSpherePBR->GetTransform()->AddChild(scratchSpherePBR->GetTransform());
	floorSpherePBR->GetTransform()->AddChild(bronzeSpherePBR->GetTransform());
	bronzeSpherePBR->GetTransform()->AddChild(paintSpherePBR->GetTransform());

	entities.push_back(cobSpherePBR);
	entities.push_back(floorSpherePBR);
	entities.push_back(scratchSpherePBR);
	entities.push_back(bronzeSpherePBR);
	entities.push_back(paintSpherePBR);

	// Save assets needed for drawing point lights
	// (Since these are just copies of the pointers,
	//  we won't need to directly delete them as 
	//  the original pointers will be cleaned up)
	lightMesh = sphereMesh;
	lightVS = vertexShader;
	lightPS = solidColorPS;

	renderer = new Renderer(
		device,
		context,
		swapChain,
		backBufferRTV,
		depthStencilView,
		this->width,
		this->height,
		sky,
		entities,
		lights,
		lightMesh,
		lightVS,
		lightPS,
		pixelShaderPBR
	);
}


// --------------------------------------------------------
// Generates the lights in the scene: 3 directional lights
// and many random point lights.
// --------------------------------------------------------
void Game::GenerateLights()
{
	// Reset
	lights.clear();

	// Setup directional lights
	Light dir1 = {};
	dir1.Type = LIGHT_TYPE_DIRECTIONAL;
	dir1.Direction = XMFLOAT3(1, -1, 1);
	dir1.Color = XMFLOAT3(0.8f, 0.8f, 0.8f);
	dir1.Intensity = 1.0f;

	Light dir2 = {};
	dir2.Type = LIGHT_TYPE_DIRECTIONAL;
	dir2.Direction = XMFLOAT3(-1, -0.25f, 0);
	dir2.Color = XMFLOAT3(0.2f, 0.2f, 0.2f);
	dir2.Intensity = 1.0f;

	Light dir3 = {};
	dir3.Type = LIGHT_TYPE_DIRECTIONAL;
	dir3.Direction = XMFLOAT3(0, -1, 1);
	dir3.Color = XMFLOAT3(0.2f, 0.2f, 0.2f);
	dir3.Intensity = 1.0f;

	// Add light to the list
	lights.push_back(dir1);
	lights.push_back(dir2);
	lights.push_back(dir3);

	// Create the rest of the lights
	while (lights.size() < lightCount)
	{
		Light point = {};
		point.Type = LIGHT_TYPE_POINT;
		point.Position = XMFLOAT3(RandomRange(-10.0f, 10.0f), RandomRange(-5.0f, 5.0f), RandomRange(-10.0f, 10.0f));
		point.Color = XMFLOAT3(RandomRange(0, 1), RandomRange(0, 1), RandomRange(0, 1));
		point.Range = RandomRange(5.0f, 10.0f);
		point.Intensity = RandomRange(0.1f, 3.0f);

		// Add to the list
		lights.push_back(point);
	}

}



// --------------------------------------------------------
// Handle resizing DirectX "stuff" to match the new window size.
// For instance, updating our projection matrix's aspect ratio.
// --------------------------------------------------------
void Game::OnResize()
{
	renderer->PreResize();

	// Handle base-level DX resize stuff
	DXCore::OnResize();

	// Update our projection matrix to match the new aspect ratio
	if (camera)
		camera->UpdateProjectionMatrix(this->width / (float)this->height);

	renderer->PostResize(
		this->width,
		this->height,
		backBufferRTV,
		depthStencilView
	);
}

// --------------------------------------------------------
// Update your game here - user input, move objects, AI, etc.
// --------------------------------------------------------
void Game::Update(float deltaTime, float totalTime)
{
	// get input
	Input& input = Input::GetInstance();

	int yPos = entities[0]->GetTransform()->GetPosition().y;
	if (yPos == 2 || yPos == -2) {
		interval = -interval;
	}

	// move entities
	entities[0]->GetTransform()->MoveRelative(0.001, 0, 0);
	entities[0]->GetTransform()->MoveAbsolute(0, interval, 0);
	entities[0]->GetTransform()->Rotate(0, 0.005, 0);
	entities[1]->GetTransform()->Rotate(0, 0.02, 0);
	entities[2]->GetTransform()->Rotate(0, 0.01, 0);
	entities[3]->GetTransform()->Rotate(0.02, 0, 0);

	//update the GUI
	UpdateGUI(deltaTime, input);

	// Update the camera
	camera->Update(deltaTime);

	// Check individual input
	if (input.KeyDown(VK_ESCAPE)) Quit();
	if (input.KeyPress(VK_TAB)) GenerateLights();

	// PhysX
	mScene->simulate(1.0f / 60.0f);
	mScene->fetchResults(true);
}

// --------------------------------------------------------
// Clear the screen, redraw everything, present to the user
// --------------------------------------------------------
void Game::Draw(float deltaTime, float totalTime)
{
	renderer->Render(camera);
}

void Game::UpdateGUI(float dt, Input& input)
{
	// Reset input manager's gui state so we don�t
	// taint our own input (you'll uncomment later)
	input.SetGuiKeyboardCapture(false);
	input.SetGuiMouseCapture(false);

	// Set io info
	ImGuiIO& io = ImGui::GetIO();
	io.DeltaTime = dt;
	io.DisplaySize.x = (float)this->width;
	io.DisplaySize.y = (float)this->height;
	io.KeyCtrl = input.KeyDown(VK_CONTROL);
	io.KeyShift = input.KeyDown(VK_SHIFT);
	io.KeyAlt = input.KeyDown(VK_MENU);
	io.MousePos.x = (float)input.GetMouseX();
	io.MousePos.y = (float)input.GetMouseY();
	io.MouseDown[0] = input.MouseLeftDown();
	io.MouseDown[1] = input.MouseRightDown();
	io.MouseDown[2] = input.MouseMiddleDown();
	io.MouseWheel = input.GetMouseWheel();
	input.GetKeyArray(io.KeysDown, 256);

	// Reset the frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	// Determine new input capture (you'll uncomment later)
	input.SetGuiKeyboardCapture(io.WantCaptureKeyboard);
	input.SetGuiMouseCapture(io.WantCaptureMouse);

	// creat windows
	UpdateStatsWindow(io.Framerate);
	UpdateSceneWindow();
}

void Game::UpdateStatsWindow(int framerate)
{
	ImGui::Begin("Program Stats");

	ImGui::Text(ConcatStringAndInt("Framerate: ", framerate).c_str());

	if (ImGui::CollapsingHeader("Window Properties")) {
		ImGui::Text(ConcatStringAndInt("Width: ", this->width).c_str());
		ImGui::Text(ConcatStringAndInt("Height: ", this->height).c_str());
		ImGui::Text(ConcatStringAndFloat("Aspect Ratio: ", (this->width / (float)this->height)).c_str());
	}

	if (ImGui::CollapsingHeader("Scene Properties")) {
		ImGui::Text(ConcatStringAndInt("Number of Entities: ", entities.size()).c_str());
		ImGui::Text(ConcatStringAndInt("Number of Lights: ", lightCount).c_str());
	}

	ImGui::End();
}

void Game::UpdateSceneWindow()
{
	ImGui::Begin("Scene");

	if (ImGui::CollapsingHeader("Entities")) {
		// number of entities
		ImGui::Text(ConcatStringAndInt("Number of Entities: ", entities.size()).c_str());

		const char* meshTitles[] = { "Sphere", "Helix", "Cube", "Cone" };

		const char* materialTitles[] = {
			"Cobblestone",
			"Floor",
			"Paint",
			"Scratched",
			"Bronze",
			"Rough",
			"Wood",
			"Cobblestone - PBR",
			"Floor - PBR",
			"Paint - PBR",
			"Scratched - PBR",
			"Bronze - PBR",
			"Rough - PBR",
			"Wood - PBR"
		};

		// specific entity headers
		for (int i = 0; i < entities.size(); i++)
		{
			GenerateEntitiesHeader(i, meshTitles, materialTitles);
		}
	}

	if (ImGui::CollapsingHeader("Lights")) {
		// number of lights slider
		ImGui::SliderInt("Number of Lights", &lightCount, 0, 64);

		// specific light headers
		for (int i = 0; i < lightCount; i++)
		{
			GenerateLightsHeader(i);
		}
	}

	GenerateCameraHeader();

	if (ImGui::CollapsingHeader("Materials")) {
		// number of materials
		ImGui::Text(ConcatStringAndInt("Number of Materials: ", materials.size()).c_str());

		const char* textureTitles[] = {
			"Cobblestone A", "Cobblestone N", "Cobblestone R", "Cobblestone M",
			"Floor A", "Floor N", "Floor R", "Floor M",
			"Paint A", "Paint N", "Paint R", "Paint M",
			"Scratched A", "Scratched N", "Scratched R", "Scratched M",
			"Bronze A", "Bronze N", "Bronze R", "Bronze M",
			"Rough A", "Rough N", "Rough R", "Rough M",
			"Wood A", "Wood N", "Wood R", "Wood M",
		};

		// select specific material headers
		for (int i = 0; i < materials.size(); i++)
		{
			GenerateMaterialsHeader(i, textureTitles);
		}
	}

	ImGui::End();
}

void Game::GenerateEntitiesHeader(int i, const char* meshTitles[], const char* materialTitles[])
{
	if (ImGui::CollapsingHeader(ConcatStringAndInt("Entity ", i + 1).c_str())) {
		// change mesh
		int currentMesh = FindIndex(meshes, entities[i]->GetMesh());
		ImGui::Combo(ConcatStringAndInt("Mesh##E", i).c_str(), &currentMesh, meshTitles, meshes.size());
		entities[i]->SetMesh(meshes[currentMesh]);

		// change materials
		int currentMaterial = FindIndex(materials, entities[i]->GetMaterial());
		ImGui::Combo(ConcatStringAndInt("Material##E", i).c_str(), &currentMaterial, materialTitles, materials.size());
		entities[i]->SetMaterial(materials[currentMaterial]);

		// transform controls
		ImGui::Text("Transform:");

		XMFLOAT3 pos = entities[i]->GetTransform()->GetPosition();
		ImGui::InputFloat3(ConcatStringAndInt("Position##E", i).c_str(), &pos.x);
		entities[i]->GetTransform()->SetPosition(pos.x, pos.y, pos.z);

		XMFLOAT3 rot = entities[i]->GetTransform()->GetPitchYawRoll();
		ImGui::SliderFloat3(ConcatStringAndInt("Rotation##E", i).c_str(), &rot.x, 0.0f, 6.28319f);
		entities[i]->GetTransform()->SetRotation(rot.x, rot.y, rot.z);

		XMFLOAT3 scale = entities[i]->GetTransform()->GetScale();
		ImGui::InputFloat3(ConcatStringAndInt("Scale##E", i).c_str(), &scale.x);
		entities[i]->GetTransform()->SetScale(scale.x, scale.y, scale.z);

		// add/remove children
		if (ImGui::CollapsingHeader(ConcatStringAndInt("Add/Remove Children##E", i + 1).c_str())) {
			for (int j = 0; j < entities.size(); j++) {
				if (i == j)
					continue;

				Transform* parentTransform = entities[i]->GetTransform();
				Transform* childTransform = entities[j]->GetTransform();

				bool isChild = parentTransform->IndexOfChild(childTransform) > -1;
				ImGui::Checkbox(
					(ConcatStringAndInt("Child ", j + 1) + ConcatStringAndInt("##", i + 1)).c_str(), &isChild);
				if (!isChild) {
					parentTransform->RemoveChild(childTransform);
				}
				else {
					parentTransform->AddChild(childTransform);
				}
			}
		}
	}
}

void Game::GenerateLightsHeader(int i)
{

	if (ImGui::CollapsingHeader(ConcatStringAndInt("Light ", i + 1).c_str())) {
		// type buttons
		ImGui::RadioButton(ConcatStringAndInt("Directional##", i).c_str(), &lights[i].Type, LIGHT_TYPE_DIRECTIONAL); ImGui::SameLine();
		ImGui::RadioButton(ConcatStringAndInt("Point##", i).c_str(), &lights[i].Type, LIGHT_TYPE_POINT); ImGui::SameLine();
		ImGui::RadioButton(ConcatStringAndInt("Spot##", i).c_str(), &lights[i].Type, LIGHT_TYPE_SPOT);

		switch (lights[i].Type) {
		case LIGHT_TYPE_SPOT:
			ImGui::SliderFloat(ConcatStringAndInt("Spot Falloff##", i).c_str(), &lights[i].SpotFalloff, 0, 20);
		case LIGHT_TYPE_DIRECTIONAL:
			ImGui::SliderFloat3(ConcatStringAndInt("Direction##", i).c_str(), &lights[i].Direction.x, -1, 1);
			break;
		case LIGHT_TYPE_POINT:
			ImGui::SliderFloat(ConcatStringAndInt("Range##", i).c_str(), &lights[i].Range, 0, 20);
			break;
		}

		ImGui::InputFloat3(ConcatStringAndInt("Position##L", i).c_str(), &lights[i].Position.x);
		ImGui::SliderFloat(ConcatStringAndInt("Intensity##", i).c_str(), &lights[i].Intensity, 0, 5);
		ImGui::ColorEdit3(ConcatStringAndInt("Color##L", i).c_str(), &lights[i].Color.x);
	}
}

void Game::GenerateCameraHeader()
{
	if (ImGui::CollapsingHeader("Cameras")) {
		// camera information - only one right now
		ImGui::Text("Current Camera: First-Person Controllable");

		// set position
		XMFLOAT3 pos = camera->GetTransform()->GetPosition();
		ImGui::InputFloat3("Position##C", &pos.x);
		camera->GetTransform()->SetPosition(pos.x, pos.y, pos.z);

		// set rotation
		XMFLOAT3 rot = camera->GetTransform()->GetPitchYawRoll();
		ImGui::SliderFloat2("Rotation##C", &rot.x, 0.0f, 6.28319f);
		camera->GetTransform()->SetRotation(rot.x, rot.y, rot.z);
	}
}

void Game::GenerateMaterialsHeader(int i, const char* textureTitles[])
{
	if (ImGui::CollapsingHeader(ConcatStringAndInt("Material ", i + 1).c_str())) {
		// toggle PBR
		bool isPBR = materials[i]->GetPS() == pixelShaderPBR;
		ImGui::Checkbox(ConcatStringAndInt("PBR##", i).c_str(), &isPBR);
		if (isPBR) {
			materials[i]->SetPS(pixelShaderPBR);
		}
		else {
			materials[i]->SetPS(pixelShader);

			float shininess = materials[i]->GetShininess();
			ImGui::SliderFloat(ConcatStringAndInt("Shininess##Ma", i).c_str(), &shininess, 0.0f, 256.0f);
			materials[i]->SetShininess(shininess);
		}

		DirectX::XMFLOAT4 color = materials[i]->GetColor();
		ImGui::ColorEdit3(ConcatStringAndInt("Color##Ma", i).c_str(), &color.x);
		materials[i]->SetColor(color);

		// display and edit textures
		ImGui::Text("Textures: ");
		ImVec2 size = ImVec2(100, 100);
		ImVec2 uv_min = ImVec2(0.0f, 0.0f);                 // Top-left
		ImVec2 uv_max = ImVec2(1.0f, 1.0f);                 // Lower-right
		ImVec4 tint_col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);   // No tint
		ImVec4 border_col = ImVec4(1.0f, 1.0f, 1.0f, 0.5f); // 50% opaque white

		ImTextureID albedo = materials[i]->GetAlbedo().Get();
		ImGui::Image(albedo, size, uv_min, uv_max, tint_col, border_col); ImGui::SameLine();

		ImTextureID normal = materials[i]->GetNormal().Get();
		ImGui::Image(normal, size, uv_min, uv_max, tint_col, border_col); ImGui::SameLine();

		ImTextureID roughness = materials[i]->GetRoughness().Get();
		ImGui::Image(roughness, size, uv_min, uv_max, tint_col, border_col); ImGui::SameLine();

		ImTextureID metal = materials[i]->GetMetal().Get();
		ImGui::Image(metal, size, uv_min, uv_max, tint_col, border_col);

		int currentAlbedo = FindIndex(textures, materials[i]->GetAlbedo());
		ImGui::Combo(ConcatStringAndInt("Albedo##Ma", i).c_str(), &currentAlbedo, textureTitles, textures.size());
		materials[i]->SetAlbedo(textures[currentAlbedo]);

		int currentNormal = FindIndex(textures, materials[i]->GetNormal());
		ImGui::Combo(ConcatStringAndInt("Normal##Ma", i).c_str(), &currentNormal, textureTitles, textures.size());
		materials[i]->SetNormal(textures[currentNormal]);

		int currentRough = FindIndex(textures, materials[i]->GetRoughness());
		ImGui::Combo(ConcatStringAndInt("Roughness##Ma", i).c_str(), &currentRough, textureTitles, textures.size());
		materials[i]->SetRoughness(textures[currentRough]);

		int currentMetal = FindIndex(textures, materials[i]->GetMetal());
		ImGui::Combo(ConcatStringAndInt("Metal##Ma", i).c_str(), &currentMetal, textureTitles, textures.size());
		materials[i]->SetMetal(textures[currentMetal]);
	}
}

std::string Game::ConcatStringAndInt(std::string str, int i)
{
	std::string numToString = std::to_string(i);
	return str + numToString;
}

std::string Game::ConcatStringAndFloat(std::string str, float f)
{
	std::string numToString = std::to_string(f);
	return str + numToString;
}
