#include "EnemyCharacter.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

// ==================================================
// Constructor / Initialize
// ==================================================

AEnemyCharacter::AEnemyCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
	AIControllerClass = AAIController::StaticClass();

	GetCharacterMovement()->DefaultLandMovementMode = MOVE_Walking;
	GetCharacterMovement()->bOrientRotationToMovement = false;
	GetCharacterMovement()->bCanWalkOffLedges = true;

	GetCharacterMovement()->JumpZVelocity = 450.0f;
	GetCharacterMovement()->AirControl = 0.8f;
}

void AEnemyCharacter::BeginPlay()
{
	Super::BeginPlay();

	CurrentHP = MaxHP;
	InitialSpawnLocation = GetActorLocation();
	InitialSpawnRotation = GetActorRotation();

	GetCharacterMovement()->SetMovementMode(MOVE_Walking);

	TargetPlayer = UGameplayStatics::GetPlayerPawn(this, 0);
	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("BeginPlay Target: %s"), *GetNameSafe(TargetPlayer));

	EnterIdleState();
}

// ==================================================
// Tick / Main Update
// ==================================================

void AEnemyCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	const bool bIsCurrentlyFalling = GetCharacterMovement()->IsFalling();

	// 段差から落ち始めた瞬間を検出する
	if (bIsCurrentlyFalling && !bWasFallingLastFrame)
	{
		// 自発ジャンプではない落下 = 段差落下として扱う
		if (!bEnemyJumpStarted)
		{
			bLedgeFallDetected = true;
			INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy ledge fall detected"));
		}
	}

	bWasFallingLastFrame = bIsCurrentlyFalling;

	// 行動不能状態ではAI更新しない
	if (!TargetPlayer || bIsDead || bIsRespawning)
	{
		return;
	}

	const float Distance = FVector::Dist(GetActorLocation(), TargetPlayer->GetActorLocation());

	// 攻撃距離内なら移動を止めて攻撃
	if (Distance <= AttackDistance)
	{
		GetCharacterMovement()->StopMovementImmediately();
		TryAttack();
		return;
	}

	// 追跡距離内なら移動とジャンプ判断を行う
	if (Distance <= ChaseDistance)
	{
		MoveToPlayer(DeltaTime);
		TryJumpToTarget();
		return;
	}
}

// ==================================================
// AI Behavior
// ==================================================

void AEnemyCharacter::MoveToPlayer(float DeltaTime)
{
	if (!TargetPlayer)
	{
		return;
	}

	FVector Direction = TargetPlayer->GetActorLocation() - GetActorLocation();
	Direction.Z = 0.0f;

	if (!Direction.Normalize())
	{
		return;
	}

	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Direction = %s"), *Direction.ToString());

	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	GetCharacterMovement()->MaxWalkSpeed = MoveSpeed;

	AddMovementInput(Direction, 1.0f, true);

	const FRotator LookAtRotation = Direction.Rotation();
	SetActorRotation(FRotator(0.0f, LookAtRotation.Yaw + 180.0f, 0.0f));
}

void AEnemyCharacter::TryAttack()
{
	if (!bCanAttack)
	{
		return;
	}

	bCanAttack = false;

	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy Attack"));

	PlayAttackMontage();

	GetWorldTimerManager().SetTimer(
		AttackCooldownTimer,
		[this]()
		{
			bCanAttack = true;
		},
		AttackInterval,
		false
	);
}

void AEnemyCharacter::PlayAttackMontage()
{
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance || !AttackMontage)
	{
		return;
	}

	AnimInstance->Montage_Play(AttackMontage);
}

void AEnemyCharacter::TryJumpToTarget()
{
	if (!TargetPlayer || !bCanJumpToTarget)
	{
		return;
	}

	// 段差落下後のロック中はジャンプ禁止
	if (bJumpLockedAfterLedgeFall)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Jump blocked: ledge-fall lock"));
		return;
	}

	// 空中ではジャンプしない
	if (!GetCharacterMovement()->IsMovingOnGround())
	{
		return;
	}

	const float HeightDelta = TargetPlayer->GetActorLocation().Z - GetActorLocation().Z;
	const float HorizontalDistance = FVector::Dist2D(
		GetActorLocation(),
		TargetPlayer->GetActorLocation()
	);

	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("HeightDelta = %f / HorizontalDistance = %f"), HeightDelta, HorizontalDistance);

	// プレイヤーが一定以上上にいて、かつ近距離にいる時のみジャンプする
	if (HeightDelta > JumpHeightThreshold && HorizontalDistance < 120.0f)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy Jump!"));

		bEnemyJumpStarted = true;
		bCanJumpToTarget = false;

		Jump();

		GetWorldTimerManager().SetTimer(
			JumpCooldownTimer,
			[this]()
			{
				bCanJumpToTarget = true;
			},
			JumpCooldown,
			false
		);
	}
}

// ==================================================
// Damage
// ==================================================

void AEnemyCharacter::ReceiveAttackDamage(float DamageAmount)
{
	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("ReceiveAttackDamage called. DamageAmount = %f, CurrentHP(before) = %d"), DamageAmount, CurrentHP);

	if (bIsDead || bIsRespawning)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy already dead or respawning"));
		return;
	}

	if (bIsDead)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy already dead"));
		return;
	}

	if (!bCanTakeDamage)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy cannot take damage now"));
		return;
	}

	if (DamageAmount <= 0.0f)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("DamageAmount <= 0"));
		return;
	}

	CurrentHP -= FMath::RoundToInt(DamageAmount);
	CurrentHP = FMath::Max(CurrentHP, 0);

	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy HP(after) = %d"), CurrentHP);

	bCanTakeDamage = false;
	GetWorldTimerManager().SetTimer(
		DamageInvincibleTimer,
		[this]()
		{
			bCanTakeDamage = true;
		},
		DamageInvincibleTime,
		false
	);

	if (CurrentHP <= 0)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enter BlowawayState"));
		EnterBlowawayState();
	}
	else
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enter DamageState"));
		EnterDamageState();
	}
}

// ==================================================
// State Transition
// ==================================================

void AEnemyCharacter::EnterIdleState()
{
	if (bIsDead)
	{
		return;
	}

	CurrentState = EEnemyState::Idle;
}

void AEnemyCharacter::EnterDamageState()
{
	if (bIsDead)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("EnterDamageState blocked: already dead"));
		return;
	}

	CurrentState = EEnemyState::Damage;
	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("EnterDamageState called"));

	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Damage: AnimInstance is null"));
		return;
	}

	if (!DamageMontage)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Damage: DamageMontage is null"));
		return;
	}

	const float Played = AnimInstance->Montage_Play(DamageMontage);
	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Damage Montage_Play result = %f"), Played);

	AnimInstance->OnMontageEnded.RemoveDynamic(this, &AEnemyCharacter::OnDamageMontageEnded);
	AnimInstance->OnMontageEnded.AddDynamic(this, &AEnemyCharacter::OnDamageMontageEnded);
}

void AEnemyCharacter::EnterBlowawayState()
{
	if (bIsDead)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("EnterBlowawayState blocked: already dead"));
		return;
	}

	bIsDead = true;
	CurrentState = EEnemyState::Blowaway;

	INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("EnterBlowawayState called"));

	GetCharacterMovement()->DisableMovement();

	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	float DelayBeforeDeathBlink = 0.5f;

	if (!AnimInstance)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Blowaway: AnimInstance is null"));
	}
	else if (!BlowawayMontage)
	{
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Blowaway: BlowawayMontage is null"));
	}
	else
	{
		const float Played = AnimInstance->Montage_Play(BlowawayMontage);
		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Blowaway Montage_Play result = %f"), Played);

		if (Played > 0.0f)
		{
			DelayBeforeDeathBlink = Played;
		}
	}

	GetWorldTimerManager().ClearTimer(StartDeathBlinkTimerHandle);
	GetWorldTimerManager().SetTimer(
		StartDeathBlinkTimerHandle,
		this,
		&AEnemyCharacter::StartDeathBlink,
		DelayBeforeDeathBlink,
		false
	);
}

// ==================================================
// Animation Callback
// ==================================================

void AEnemyCharacter::OnDamageMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
	if (bIsDead)
	{
		return;
	}

	if (Montage == DamageMontage)
	{
		EnterIdleState();
	}

	if (UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance())
	{
		AnimInstance->OnMontageEnded.RemoveDynamic(this, &AEnemyCharacter::OnDamageMontageEnded);
	}
}

// ==================================================
// Respawn Sequence
// ==================================================

void AEnemyCharacter::StartDeathBlink()
{
	bIsRespawning = true;
	bIsRespawnBlinking = false;

	GetWorldTimerManager().ClearTimer(DeathBlinkTimerHandle);
	GetWorldTimerManager().SetTimer(
		DeathBlinkTimerHandle,
		this,
		&AEnemyCharacter::ToggleDeathBlinkVisibility,
		BlinkInterval,
		true
	);

	GetWorldTimerManager().ClearTimer(HideEnemyTimerHandle);
	GetWorldTimerManager().SetTimer(
		HideEnemyTimerHandle,
		this,
		&AEnemyCharacter::HideEnemy,
		DeathBlinkDuration,
		false
	);
}

void AEnemyCharacter::ToggleDeathBlinkVisibility()
{
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetVisibility(!MeshComp->IsVisible(), true);
	}
}

void AEnemyCharacter::HideEnemy()
{
	GetWorldTimerManager().ClearTimer(DeathBlinkTimerHandle);

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetVisibility(false, true);
	}

	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetActorHiddenInGame(true);

	const float HiddenDelay = FMath::Max(0.0f, TotalRespawnTime - DeathBlinkDuration - RespawnBlinkDuration);

	GetWorldTimerManager().ClearTimer(RespawnTimerHandle);
	GetWorldTimerManager().SetTimer(
		RespawnTimerHandle,
		this,
		&AEnemyCharacter::RespawnEnemy,
		HiddenDelay,
		false
	);
}

void AEnemyCharacter::RespawnEnemy()
{
	SetActorLocation(InitialSpawnLocation);
	SetActorRotation(InitialSpawnRotation);

	SetActorHiddenInGame(false);

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetVisibility(false, true);
	}

	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);

	CurrentHP = MaxHP;
	bIsDead = false;
	CurrentState = EEnemyState::Idle;

	StartRespawnBlink();
}

void AEnemyCharacter::StartRespawnBlink()
{
	bIsRespawnBlinking = true;

	GetWorldTimerManager().ClearTimer(RespawnBlinkTimerHandle);
	GetWorldTimerManager().SetTimer(
		RespawnBlinkTimerHandle,
		this,
		&AEnemyCharacter::ToggleRespawnBlinkVisibility,
		BlinkInterval,
		true
	);

	GetWorldTimerManager().ClearTimer(FinishRespawnTimerHandle);
	GetWorldTimerManager().SetTimer(
		FinishRespawnTimerHandle,
		this,
		&AEnemyCharacter::FinishRespawn,
		RespawnBlinkDuration,
		false
	);
}

void AEnemyCharacter::ToggleRespawnBlinkVisibility()
{
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetVisibility(!MeshComp->IsVisible(), true);
	}
}

void AEnemyCharacter::FinishRespawn()
{
	GetWorldTimerManager().ClearTimer(RespawnBlinkTimerHandle);

	SetActorHiddenInGame(false);

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetVisibility(true, true);
	}

	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);

	bCanTakeDamage = true;
	bIsRespawning = false;
	bIsRespawnBlinking = false;

	EnterIdleState();
}

// ==================================================
// Landing Event
// ==================================================

void AEnemyCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	// 自発ジャンプ状態を解除
	bEnemyJumpStarted = false;
	bWasFallingLastFrame = false;

	// 段差落下後の着地時は一定時間ジャンプを禁止する
	if (bLedgeFallDetected)
	{
		bLedgeFallDetected = false;
		bJumpLockedAfterLedgeFall = true;

		INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy landed after ledge fall -> jump locked"));

		GetWorldTimerManager().ClearTimer(LedgeFallJumpLockTimer);
		GetWorldTimerManager().SetTimer(
			LedgeFallJumpLockTimer,
			[this]()
			{
				bJumpLockedAfterLedgeFall = false;
				INSEKI_ENEMY_DEBUG_LOG(Warning, TEXT("Enemy jump lock released"));
			},
			LedgeFallJumpLockTime,
			false
		);
	}
}