#include "ThirdPersonCamera.h"

#include "Input.h"

using namespace DirectX;

ThirdPersonCamera::ThirdPersonCamera(GameEntity* entity, float aspectRatio)
{
	this->entity = entity;

	pivot = new Transform();
	XMFLOAT3 entityPos = entity->GetTransform()->GetPosition();
	pivot->SetPosition(entityPos.x, entityPos.y, entityPos.z);

	camera = new Camera(
		0, 0, -15,	// Position
		3.0f,		// Move speed
		1.0f,		// Mouse look
		aspectRatio); // Aspect ratio

	pivot->AddChild(camera->GetTransform());
}

ThirdPersonCamera::~ThirdPersonCamera()
{
	delete camera;
	delete pivot;
}

Camera* ThirdPersonCamera::GetCamera()
{
	return camera;
}

void ThirdPersonCamera::Update(float dt)
{
	Transform* cameraTransform = camera->GetTransform();
	
	pivot->RemoveChild(cameraTransform);

	// Current speed
	float speed = dt * 3.0f;

	// Get the input manager instance
	Input& input = Input::GetInstance();

	// Movement
	if (input.KeyDown(VK_RIGHT)) { pivot->Rotate(0, 0.001f, 0); }
	if (input.KeyDown(VK_LEFT)) { pivot->Rotate(0, -0.001f, 0); }
	if (input.KeyDown(VK_UP)) { pivot->Rotate(0.001f, 0, 0); }
	if (input.KeyDown(VK_DOWN)) { pivot->Rotate(-0.001f, 0, 0); }

	pivot->AddChild(cameraTransform);

	XMFLOAT3 entityPos = entity->GetTransform()->GetPosition();
	XMFLOAT3 prevPos = pivot->GetPosition();
	pivot->SetPosition(entityPos.x, entityPos.y, entityPos.z);
	XMFLOAT3 pos = pivot->GetPosition();

	cameraTransform->MoveRelative(pos.x - prevPos.x, pos.y - prevPos.y, pos.z - prevPos.z);

	camera->UpdateViewMatrix();
}
