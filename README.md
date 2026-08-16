# YuzoraClient Development Plan

YuzoraClient は、Minecraft Bedrock Edition for Windows を対象とした **Native DLL Client** として開発します。

C++ / CMake / MSVC を使用し、Minecraft の内部処理を扱うための SDK、Hook、Event、Rendering、Module System などを段階的に実装していきます。

単に機能を追加していくのではなく、Minecraft のアップデートへ追従しやすく、長期的に拡張できるクライアント基盤を構築することを目標とします。

> [!NOTE]
> 現在は開発初期段階です。  
> Minecraft のアップデートによって内部構造、関数、Signature などが変更される可能性があるため、バージョン依存部分を可能な限り分離した設計を採用します。

---

# Development Environment

基本的な開発環境は以下を予定しています。

- Windows 11
- Visual Studio 2022
- C++23
- MSVC
- CMake
- x64
- Git / GitHub

必要に応じて以下も利用します。

- Ninja
- Ghidra
- x64dbg
- Visual Studio Debugger

---

# Design Goals

YuzoraClient では、以下を重要な設計方針とします。

- Minecraft のバージョン依存処理を分離する
- Module から Signature や生アドレスを直接扱わない
- Module から Hook 実装を直接扱わない
- Minecraft 内部処理は SDK を経由して利用する
- Hook で取得した処理は Event として公開する
- 各機能を Module として独立させる
- Minecraft 更新時の影響範囲を最小限にする
- 問題発生時に原因を追跡できる Diagnostics を用意する
- Config / Keybind / Rendering などの共通機能を Module から分離する

理想的な依存関係は以下です。

```text
Minecraft
    ↓
Memory / Signature
    ↓
Version Layer
    ↓
SDK
    ↓
Hooks
    ↓
Events
    ↓
Modules
    ↓
Rendering / UI
```

Module が Minecraft の内部実装を直接意識しない構造を目指します。

---

# Development Flow

YuzoraClient は、大きく3段階に分けて開発します。

```text
YuzoraClient.dll を作成
        ↓
テスト用 EXE で DLL をロード
        ↓
Minecraft 向け基盤を段階的に実装
```

いきなり Minecraft 内部の処理を大量に実装するのではなく、まず DLL と Client Lifecycle を安定させ、その上に Minecraft 固有の機能を追加していきます。

---

# Phase 1 - DLL Foundation

最初に通常の Windows DLL として YuzoraClient を構築します。

```text
YuzoraClient/
├─ CMakeLists.txt
├─ src/
│  ├─ dllmain.cpp
│  ├─ Client.cpp
│  └─ Client.hpp
│
├─ include/
│
└─ tests/
   └─ LoaderTest.cpp
```

CMake では `SHARED` ライブラリとしてビルドします。

```cmake
add_library(YuzoraClient SHARED
    src/dllmain.cpp
    src/Client.cpp
)
```

生成物：

```text
YuzoraClient.dll
```

---

## DllMain

`DllMain` では最低限の処理のみ行います。

```cpp
#include <Windows.h>

BOOL APIENTRY DllMain(
    HMODULE module,
    DWORD reason,
    LPVOID
) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }

    return TRUE;
}
```

重い初期化処理は `DllMain` 内では行わず、Client 側へ分離します。

```text
DllMain
   ↓
Client
   ↓
Subsystems
```

Client が各 Subsystem の Lifecycle を管理します。

```text
Client
├─ initialize()
├─ shutdown()
└─ subsystem management
```

---

# Phase 2 - DLL Loader Test

Minecraft に組み込む前に、自作のテスト EXE から DLL が正常にロードできることを確認します。

```text
LoaderTest.exe
      ↓
LoadLibrary
      ↓
YuzoraClient.dll
      ↓
YuzoraInitialize()
```

DLL から初期化関数を Export します。

```cpp
extern "C"
__declspec(dllexport)
void YuzoraInitialize();

extern "C"
__declspec(dllexport)
void YuzoraShutdown();
```

最初の目標は以下です。

```text
[YuzoraClient] Initialized
YuzoraClient loaded!
```

ここまで確認できたら Minecraft 向け処理へ進みます。

---

# Phase 3 - Minecraft Client

最終的には以下の構造を目指します。

```text
Minecraft
   │
   └─ YuzoraClient.dll
          │
          ├─ Core
          ├─ Platform
          ├─ Memory
          ├─ Version
          ├─ SDK
          ├─ Hooks
          ├─ Events
          ├─ Input
          ├─ Rendering
          ├─ Modules
          ├─ Config
          ├─ Network
          └─ Diagnostics
```

---

# Project Structure

最終的な構成は以下を予定しています。

```text
src/
├─ core/
│  ├─ Client.cpp
│  ├─ Client.hpp
│  ├─ Logger.cpp
│  ├─ Logger.hpp
│  ├─ Lifecycle.cpp
│  └─ Lifecycle.hpp
│
├─ platform/
│  └─ windows/
│     ├─ WindowsPlatform.cpp
│     └─ WindowsPlatform.hpp
│
├─ memory/
│  ├─ Memory.cpp
│  ├─ Memory.hpp
│  ├─ Scanner.cpp
│  ├─ Scanner.hpp
│  ├─ Signature.cpp
│  ├─ Signature.hpp
│  ├─ SignatureManager.cpp
│  └─ SignatureManager.hpp
│
├─ version/
│  ├─ GameVersion.cpp
│  ├─ GameVersion.hpp
│  ├─ VersionManager.cpp
│  ├─ VersionManager.hpp
│  ├─ VersionProfile.cpp
│  └─ VersionProfile.hpp
│
├─ sdk/
│  ├─ client/
│  │  └─ ClientInstance.hpp
│  │
│  ├─ actor/
│  │  ├─ Actor.hpp
│  │  ├─ Player.hpp
│  │  └─ LocalPlayer.hpp
│  │
│  ├─ world/
│  │  ├─ Level.hpp
│  │  ├─ Block.hpp
│  │  └─ Dimension.hpp
│  │
│  ├─ item/
│  │  ├─ Item.hpp
│  │  ├─ ItemStack.hpp
│  │  └─ Inventory.hpp
│  │
│  ├─ network/
│  │  └─ Packet.hpp
│  │
│  └─ math/
│     ├─ Vec2.hpp
│     ├─ Vec3.hpp
│     ├─ AABB.hpp
│     └─ Matrix.hpp
│
├─ hooks/
│  ├─ Hook.cpp
│  ├─ Hook.hpp
│  ├─ HookManager.cpp
│  ├─ HookManager.hpp
│  └─ impl/
│
├─ events/
│  ├─ Event.hpp
│  ├─ EventBus.cpp
│  ├─ EventBus.hpp
│  └─ types/
│     ├─ TickEvent.hpp
│     ├─ RenderEvent.hpp
│     ├─ KeyEvent.hpp
│     ├─ MouseEvent.hpp
│     └─ PacketEvent.hpp
│
├─ input/
│  ├─ Keyboard.cpp
│  ├─ Keyboard.hpp
│  ├─ Mouse.cpp
│  ├─ Mouse.hpp
│  ├─ KeybindManager.cpp
│  └─ KeybindManager.hpp
│
├─ modules/
│  ├─ Module.cpp
│  ├─ Module.hpp
│  ├─ ModuleManager.cpp
│  ├─ ModuleManager.hpp
│  │
│  ├─ combat/
│  ├─ movement/
│  ├─ player/
│  ├─ visual/
│  ├─ world/
│  └─ misc/
│
├─ rendering/
│  ├─ Renderer.cpp
│  ├─ Renderer.hpp
│  ├─ Font.cpp
│  ├─ Font.hpp
│  ├─ DrawList.cpp
│  ├─ DrawList.hpp
│  └─ ui/
│
├─ config/
│  ├─ ConfigManager.cpp
│  ├─ ConfigManager.hpp
│  ├─ Profile.cpp
│  ├─ Profile.hpp
│  ├─ Serialization.cpp
│  └─ Serialization.hpp
│
├─ network/
│  └─ inspector/
│     ├─ PacketInspector.cpp
│     └─ PacketInspector.hpp
│
├─ diagnostics/
│  ├─ Diagnostics.cpp
│  ├─ Diagnostics.hpp
│  ├─ DebugOverlay.cpp
│  ├─ DebugOverlay.hpp
│  ├─ SignatureDiagnostics.cpp
│  └─ SignatureDiagnostics.hpp
│
└─ dllmain.cpp
```

初期段階ですべてを実装するわけではありません。

必要になったタイミングで各 Subsystem を段階的に追加していきます。

---

# Core

Core は YuzoraClient 全体の Lifecycle を管理します。

```text
DllMain
   ↓
Client
   ↓
Lifecycle
   ↓
Subsystems
```

主な役割：

```text
Client
├─ initialize()
├─ shutdown()
├─ subsystem initialization
└─ subsystem shutdown
```

Minecraft 固有の処理を `DllMain` に直接書かず、Client を起点として各システムを管理します。

---

# Memory System

Minecraft のメモリ上に存在する関数やデータへアクセスするための共通機能を管理します。

```text
Minecraft Module
       ↓
Memory Utilities
       ↓
Scanner
       ↓
Signature System
```

Module 側から Memory API を直接利用することは原則として避けます。

---

# Signature System

Minecraft はアップデートによって内部アドレスが変化するため、固定アドレスへの依存をできるだけ避けます。

例えば、

```cpp
0x7FF6A1234567
```

のような絶対アドレスを Module に直接記述しません。

代わりに Signature Scanner を利用します。

```text
Minecraft Module

48 89 5C 24 ?? 57 48 83 EC ??
       ↓
Signature Scanner
       ↓
Signature Manager
       ↓
Resolved Address
```

SignatureManager では、各 Signature を名前付きで管理します。

概念例：

```text
ClientInstance::getLocalPlayer
Actor::getPosition
Level::getActors
Renderer::present
```

利用側では、

```cpp
auto address =
    Signatures::get("ClientInstance::getLocalPlayer");
```

のような形を目指します。

各 Signature には状態を持たせます。

```text
FOUND
MISSING
AMBIGUOUS
INVALID
```

起動時には以下のような診断情報を表示できるようにします。

```text
[YuzoraClient] Signature Scan

[OK] ClientInstance::getLocalPlayer
[OK] Actor::getPosition
[OK] Level::getActors
[!!] Renderer::present

3 / 4 signatures resolved
```

---

# Version System

Minecraft のバージョン依存処理を管理する専用レイヤーを実装します。

```text
Minecraft
    ↓
GameVersion
    ↓
VersionManager
    ↓
VersionProfile
    ↓
Memory / SDK
```

Module 内に、

```cpp
if (minecraftVersion == ...) {
    // version specific code
}
```

のような条件分岐を増やさないことを重要なルールとします。

Module 側はバージョンを意識せず、

```cpp
auto* player = SDK::getLocalPlayer();
```

のように SDK を利用します。

Minecraft 更新時は、

```text
Minecraft Update
       ↓
Version / Signature 修正
       ↓
SDK 修正
       ↓
Modules は可能な限り変更しない
```

という形で追従できる構造を目標とします。

---

# SDK

SDK は Minecraft 内部のクラスやデータへアクセスするための抽象化レイヤーです。

初期段階では以下を対象にします。

```text
ClientInstance
     ↓
LocalPlayer
     ↓
Actor
     ↓
Level
```

Module 側では、

```cpp
auto* client = SDK::getClientInstance();

if (!client)
    return;

auto* player = client->getLocalPlayer();

if (!player)
    return;

auto position = player->getPosition();
```

のように利用できる形を目指します。

最終的には SDK を以下まで拡張します。

```text
SDK
├─ Client
├─ Actor
├─ Player
├─ LocalPlayer
├─ Level
├─ Block
├─ Dimension
├─ Item
├─ ItemStack
├─ Inventory
├─ Network
└─ Math
```

重要なルール：

> Module は Signature、Offset、生アドレスなどを直接扱わず、Minecraft の情報へアクセスするときは SDK を利用する。

---

# Hook System

Minecraft の各処理へ接続するための Hook System を実装します。

```text
Minecraft Function
       ↓
Yuzora Hook
       ↓
Event
       ↓
Original Function
```

例えば Tick Hook では、

```cpp
void hookedTick() {
    EventBus::dispatch(TickEvent{});

    originalTick();
}
```

のように Minecraft の処理を Event へ変換します。

Module が Hook を直接管理することは避けます。

```text
Minecraft
    ↓
HookManager
    ↓
EventBus
    ↓
Modules
```

---

# Event System

Minecraft の処理を Event として公開します。

初期段階では以下を予定しています。

```text
TickEvent
RenderEvent
KeyEvent
MouseEvent
PacketEvent
```

将来的には、

```text
PacketSendEvent
PacketReceiveEvent
WorldJoinEvent
WorldLeaveEvent
ActorAddedEvent
ActorRemovedEvent
```

などへ拡張します。

Module は必要な Event のみを受け取ります。

```text
Minecraft Tick
      ↓
Hook
      ↓
TickEvent
      ↓
EventBus
      ↓
Subscribed Modules
```

これによって Hook と Module を疎結合に保ちます。

---

# Input System

Keyboard / Mouse / Keybind の処理を Module から分離します。

```text
Minecraft Input
      ↓
Input System
      ↓
KeybindManager
      ↓
Modules
```

Module が個別に Windows API を直接呼び出してキー状態を取得する構造は避けます。

将来的には、

```text
Toggle
Hold
Press
Release
```

などの Keybind Mode に対応できる構造を目指します。

---

# Module System

各機能は独立した Module として実装します。

基本構造：

```cpp
class Module {
public:
    virtual ~Module() = default;

    virtual void onEnable() {}
    virtual void onDisable() {}

    virtual void onTick() {}
    virtual void onRender() {}

    void setEnabled(bool enabled);

    [[nodiscard]]
    bool isEnabled() const;

private:
    bool enabled_ = false;
};
```

ModuleManager がすべての Module を管理します。

```text
ModuleManager
├─ Combat
├─ Movement
├─ Player
├─ Visual
├─ World
└─ Misc
```

Module は可能な限り独立させます。

Module 内から、

```text
Raw Memory
Signature Scanner
Hook Implementation
Version-specific Logic
```

へ直接依存しない設計とします。

---

# Rendering System

Minecraft の描画処理へ接続し、YuzoraClient 独自の HUD / UI を表示できるようにします。

初期目標：

```text
YuzoraClient

FPS: 165
XYZ: 123 64 -320
```

Rendering System は Module と描画 API の間を抽象化します。

```text
Module
   ↓
Renderer
   ↓
Minecraft Rendering
```

将来的には、

```text
HUD
ClickGUI
Module List
Keystrokes
CPS
Debug Overlay
Custom Fonts
Textures
Animations
```

などを実装予定です。

---

# Config System

Module の設定を統一して保存・読み込みできる Config System を実装します。

各 Module が独自の方法で設定ファイルを管理する構造は避けます。

```text
Modules
    ↓
ConfigManager
    ↓
Profile
    ↓
Serialization
```

概念例：

```json
{
  "modules": {
    "Zoom": {
      "enabled": true,
      "key": "C",
      "fov": 30
    }
  }
}
```

将来的には複数 Profile に対応できる構造を目指します。

```text
Default
PvP
Building
Debug
Custom
```

保存対象：

```text
Module Enabled State
Keybind
Module Settings
UI Settings
HUD Position
Profiles
```

---

# Diagnostics System

Minecraft のアップデートや内部変更によって問題が発生した場合、原因を特定しやすくするための Diagnostics System を実装します。

主な診断対象：

```text
Game Version
Client Version
Signatures
Hooks
SDK
Modules
Rendering
Events
```

Debug Overlay の例：

```text
YuzoraClient Diagnostics

Minecraft: 26.xx
Client: 0.x.x-dev

ClientInstance : OK
LocalPlayer    : OK
Level          : OK

Signatures : 42 / 43
Hooks      : 8 / 8
Modules    : 17

FPS  : 165
Tick : 60
```

Minecraft 更新後に問題が発生した場合、

```text
[ERROR]
Signature missing:

ClientInstance::getLocalPlayer
```

のように、問題のある箇所を可能な限り特定できる構造を目指します。

---

# Network / Packet Inspector

将来的には Minecraft のネットワーク処理を確認できる Packet Inspector を実装します。

```text
Minecraft Network
       ↓
Packet Event
       ↓
Packet Inspector
```

例えば、

```text
SEND    PlayerAuthInput
RECV    MovePlayer
RECV    LevelChunk
SEND    InventoryTransaction
```

のような情報を確認できる研究・デバッグ機能を目標とします。

Packet Inspector は Module System とは分離し、Debug / Research Tool として扱います。

---

# Dependency Rules

プロジェクトが大規模化しても構造を維持するため、以下を基本ルールとします。

### Modules must not directly depend on

```text
Raw Addresses
Signature Scanner
Memory Scanner
Minecraft Version Checks
Hook Implementation
Windows-specific Input APIs
```

### Modules should use

```text
SDK
Events
Renderer
Input System
Config System
```

理想：

```text
Minecraft
   ↓
Platform / Memory
   ↓
Version / Signature
   ↓
SDK
   ↓
Hooks
   ↓
Events
   ↓
Modules
```

この依存方向を可能な限り維持します。

---

# Minecraft Update Strategy

Minecraft がアップデートされた場合、以下の順番で確認します。

```text
Minecraft Update
       ↓
Version Detection
       ↓
Signature Diagnostics
       ↓
Signature Update
       ↓
Hook Verification
       ↓
SDK Verification
       ↓
Module Verification
```

理想的には、

```text
Version
Signature
SDK
```

などの基盤部分だけを更新し、多くの Module は変更せずに復旧できる構造を目指します。

---

# Roadmap

## v0.1 - Foundation

- [ ] CMake Project
- [ ] YuzoraClient.dll Build
- [ ] Client Lifecycle
- [ ] Client Initialization
- [ ] Client Shutdown
- [ ] Logger
- [ ] LoaderTest
- [ ] x64 Debug Build

---

## v0.2 - Memory / Version Foundation

- [ ] Minecraft Version Detection
- [ ] GameVersion
- [ ] VersionManager
- [ ] Memory Utilities
- [ ] Pattern Scanner
- [ ] Signature
- [ ] Signature Manager
- [ ] Signature Diagnostics

---

## v0.3 - Hooks / Events

- [ ] Hook Base
- [ ] Hook Manager
- [ ] Basic Hook Installation
- [ ] Event Base
- [ ] EventBus
- [ ] TickEvent
- [ ] RenderEvent
- [ ] KeyEvent

---

## v0.4 - SDK

- [ ] ClientInstance
- [ ] LocalPlayer
- [ ] Actor
- [ ] Level
- [ ] Position Access
- [ ] Vec2
- [ ] Vec3
- [ ] Basic SDK Diagnostics

---

## v0.5 - Rendering

- [ ] Render Hook
- [ ] Renderer
- [ ] Font Rendering
- [ ] YuzoraClient Watermark
- [ ] FPS
- [ ] Coordinates
- [ ] Debug Overlay

---

## v0.6 - Module Foundation

- [ ] Module
- [ ] ModuleManager
- [ ] Module Categories
- [ ] Input System
- [ ] KeybindManager
- [ ] ConfigManager
- [ ] Profile System
- [ ] Config Serialization

---

## v0.7 - UI

- [ ] ClickGUI
- [ ] Module List
- [ ] Module Settings
- [ ] HUD Editor
- [ ] UI Config
- [ ] Basic Animations

---

## v0.8+ - Expansion

- [ ] Keystrokes
- [ ] CPS
- [ ] Zoom
- [ ] Toggle Sprint
- [ ] Fullbright
- [ ] Additional Visual Modules
- [ ] Additional Player Modules
- [ ] Additional Movement Modules
- [ ] Packet Events
- [ ] Packet Inspector
- [ ] SDK Inspector
- [ ] Advanced Diagnostics
- [ ] Additional Modules

---

# First Milestone

最初の大きな目標は以下です。

```text
YuzoraClient.dll
       ↓
Minecraft 内でロード
       ↓
Minecraft Version Detection
       ↓
Signature Resolution
       ↓
Render Hook
       ↓
HUD Rendering
```

最初にゲーム画面上へ、

```text
YuzoraClient

FPS: 165
XYZ: 123 64 -320
```

を表示できる状態を目指します。

さらに Diagnostics で、

```text
Minecraft : OK
Signatures: OK
Render Hook: OK
LocalPlayer: OK
```

まで確認できる状態を最初の本格的なマイルストーンとします。

---

# Long-term Goal

YuzoraClient は単に Module 数を増やすことではなく、Minecraft のアップデート後でも修正しやすく、機能を継続的に追加できるクライアント基盤を目指します。

理想的な更新フロー：

```text
Minecraft Update
       ↓
Signatures / Version Layer Update
       ↓
SDK Verification
       ↓
Hooks Verification
       ↓
YuzoraClient Restored
```

最終的には、

```text
Strong Core
     +
Stable SDK
     +
Version Isolation
     +
Event-driven Modules
     +
Extensible Rendering
     +
Diagnostics
     +
Config / Profiles
     +
Research Tools
```

を備えた、長期的に開発・拡張可能な Minecraft Bedrock Edition Client を目標とします。