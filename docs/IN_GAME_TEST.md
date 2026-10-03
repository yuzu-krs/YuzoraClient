# Minecraft 実機検証手順書 (v0.5 First Milestone)

YuzoraClient.dll を実際の Minecraft Bedrock にロードし、次の 2 点を確認するまでの手順です。

1. **DebugView のログ**にバージョン検出とフック設置が出ること
2. **ゲーム画面左上**にウォーターマーク (YuzoraClient / Minecraft バージョン / FPS) が出ること

> 前提: v0.5 ビルド済み (`build\bin\Release\YuzoraClient.dll` が存在すること)。
> まだの場合は先にビルドしてください (Debug 版でも動きますが Release 推奨)。

---

## 全体の流れ

```
Step 1  DebugView を準備する           (ログを見る道具。最初に 1 回だけ)
Step 2  Minecraft を起動する
Step 3  PowerShell から DLL を注入する  (コピー&ペーストで完結)
Step 4  結果を確認する                 (DebugView のログ + ゲーム画面)
Step 5  終了する
```

所要時間: 初回は 10 分ほど、2 回目以降は 2〜3 分。

---

## Step 1: DebugView の準備 (最初に 1 回だけ)

DLL のログは `OutputDebugStringA` 経由で出るため、ゲーム画面には表示されません。
**DebugView** (Microsoft 公式の無料ツール) で受け取ります。

### 1-1. ダウンロードと展開

1. ブラウザで次の URL を開く:
   `https://learn.microsoft.com/en-us/sysinternals/downloads/debugview`
2. ページ内の **「Download DebugView」** というリンク (青字) をクリック → `DebugView.zip` がダウンロードされる
3. ダウンロードした `DebugView.zip` を**右クリック** → **「すべて展開」** → 出た画面で **「展開」** ボタンを押す
4. 展開先フォルダ (`DebugView` フォルダ) を開くと **`dbgview.exe`** がある。この場所を覚えておく

### 1-2. 起動 (ゲーム検証を始めるたびに)

1. `dbgview.exe` を**右クリック** → **「管理者として実行」** をクリック
2. 「ユーザーアカウント制御」の画面が出たら **「はい」** を押す
   → 空のログウィンドウ (`DbgView` と書かれたタイトルバー) が開く

### 1-3. 受信設定 (毎回確認)

1. ウィンドウ上部のメニューバー **「Capture」** をクリック (File の隣あたり)
2. 開いたメニューの中の **「Capture Win32」** をクリック → チェックマーク (✓) が付く
3. もう一度 **「Capture」** をクリック → **「Capture Global Win32」** をクリック → チェックマークが付く
4. ログが残っている場合は **「Edit」** メニュー → **「Clear Display」** (Ctrl+X) で消しておく

確認方法: メニューバー下部に `(Ctrl+W) Win32  (Ctrl+K) Global Win32` と
常時表示されるので、両方にチェックが付いた状態になっていれば OK です。

この DebugView ウィンドウは**開いたまま**にしておいてください。

---

## Step 2: Minecraft を起動する

1. スタートメニューを開く (**Windows キー**) → **「Minecraft」** と入力 → Enter
2. タイトル画面 (「プレイ」ボタンが見える画面) まで待つ
   - **ワールドに入る必要はありません** (タイトル画面のままで検証できます)
3. フルスクリーンになっている場合は、ウィンドウモードへ変更:
   1. タイトル画面で **「設定」** (歯車アイコン) をクリック
   2. 左側のリストから **「ビデオ」** をクリック
   3. **「フルスクリーン」** のトグルを **OFF** にクリック
   4. **Esc** キーで設定画面を閉じる

---

## Step 3: PowerShell から DLL を注入する

### 3-1. 管理者権限のターミナルを開く

1. キーボードの **Win + X** を同時に押す (またはスタートボタンを右クリック)
2. 出たメニューから **「ターミナル (管理者)」** または **「Windows PowerShell (管理者)」** をクリック
3. 「ユーザーアカウント制御」が出たら **「はい」**
4. 開いたウィンドウのタイトルバーに **「管理者」** と表示されていることを確認

### 3-2. DLL の場所 (このプロジェクトの場合)

```
C:\Users\yuzut\OneDrive\デスクトップ\YuzoraClient\build\bin\Release\YuzoraClient.dll
```

ファイルが存在するか、エクスプローラーで確認しておく。

### 3-3. ローダーを貼り付けて実行

1. 下の **```powershell から ``` までの全ブロックをドラッグで選択**し、**Ctrl + C** でコピー
2. 管理者ターミナルのウィンドウ内で**右クリック** (または Ctrl + V) で貼り付け
3. **Enter** を押す

※ 最終行のパスは自分の環境のパスに置き換えること。

```powershell
Add-Type @"
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class YuzoraLoader {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(int a, bool i, int p);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr a, int s, int t, int pr);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr d, byte[] b, int s, out int w);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr a, uint s, IntPtr start, IntPtr p, uint f, out uint t);
    [DllImport("kernel32.dll", SetLastError=true)] static extern int WaitForSingleObject(IntPtr h, int ms);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeThread(IntPtr h, out uint code);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryW(string p);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi, SetLastError=true)] static extern IntPtr GetProcAddress(IntPtr m, string n);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string n);

    public static string Inject(string dllPath, string exportName) {
        Process proc = null;
        foreach (Process p in Process.GetProcessesByName("Minecraft.Windows")) { proc = p; break; }
        if (proc == null) return "ERROR: Minecraft が起動していません";
        IntPtr h = OpenProcess(0x1F0FFF, false, proc.Id);
        if (h == IntPtr.Zero) return "ERROR: OpenProcess 失敗 (" + Marshal.GetLastWin32Error() + ") 管理者権限で実行してください";
        try {
            byte[] path = System.Text.Encoding.Unicode.GetBytes(dllPath + "\0");
            IntPtr remotePath = VirtualAllocEx(h, IntPtr.Zero, path.Length, 0x3000, 0x40);
            int written;
            if (remotePath == IntPtr.Zero || !WriteProcessMemory(h, remotePath, path, path.Length, out written))
                return "ERROR: パス書き込み失敗 (" + Marshal.GetLastWin32Error() + ")";

            IntPtr loadLibraryW = GetProcAddress(GetModuleHandleW("kernel32.dll"), "LoadLibraryW");
            uint tid;
            IntPtr th = CreateRemoteThread(h, IntPtr.Zero, 0, loadLibraryW, remotePath, 0, out tid);
            if (th == IntPtr.Zero) return "ERROR: DLL ロード失敗 (" + Marshal.GetLastWin32Error() + ")";
            WaitForSingleObject(th, 10000);
            CloseHandle(th);

            proc.Refresh();
            IntPtr remoteBase = IntPtr.Zero;
            foreach (ProcessModule m in proc.Modules)
                if (m.ModuleName.Equals("YuzoraClient.dll", StringComparison.OrdinalIgnoreCase)) { remoteBase = m.BaseAddress; break; }
            if (remoteBase == IntPtr.Zero) return "ERROR: DLL はロードされましたがモジュールが見つかりません";

            IntPtr localBase = LoadLibraryW(dllPath);
            IntPtr localFn = GetProcAddress(localBase, exportName);
            if (localFn == IntPtr.Zero) return "ERROR: エクスポート " + exportName + " が見つかりません";
            IntPtr remoteFn = new IntPtr(remoteBase.ToInt64() + (localFn.ToInt64() - localBase.ToInt64()));

            th = CreateRemoteThread(h, IntPtr.Zero, 0, remoteFn, IntPtr.Zero, 0, out tid);
            if (th == IntPtr.Zero) return "ERROR: " + exportName + " 呼び出し失敗 (" + Marshal.GetLastWin32Error() + ")";
            WaitForSingleObject(th, 10000);
            uint code;
            GetExitCodeThread(th, out code);
            CloseHandle(th);
            return code != 0 ? "OK: " + exportName + " 成功 (pid " + proc.Id + ")"
                             : "ERROR: " + exportName + " が false を返しました (DebugView のログを確認)";
        } finally { CloseHandle(h); }
    }
}
"@

[YuzoraLoader]::Inject("C:\Users\yuzut\OneDrive\デスクトップ\YuzoraClient\build\bin\Release\YuzoraClient.dll", "YuzoraInitialize")
```

### 3-4. 成功時のターミナル表示

```
OK: YuzoraInitialize 成功 (pid 12345)
```

- `Add-Type` を 2 回以上実行して「型 'YuzoraLoader' が既に存在します」系のエラーが出たら、
  ターミナルを一度閉じて新しく開き直せば OK です
- 以後、同じターミナルでは `Add-Type` の再実行は不要です
  (最後の 1 行 `[YuzoraLoader]::Inject(...)` だけで呼べます)

---

## Step 4: 結果の確認

### 4-1. DebugView のログ (確認箇所: DebugView ウィンドウ)

この**順番**でログが出ていれば正常です (バージョン番号は環境により異なります):

```
[YuzoraClient] Minecraft detected: 1.21.xxx (Minecraft.Windows.exe)
[YuzoraClient] Signature Scan
[YuzoraClient] 0 / 0 signatures resolved
[YuzoraClient] Production SDK functions: none resolvable yet (signatures pending reverse engineering)
[YuzoraClient] SDK diagnostics
[YuzoraClient] [!!] ClientInstance::getInstance - not resolved
[YuzoraClient] [!!] ClientInstance::getLocalPlayer - not resolved
[YuzoraClient] [!!] ClientInstance::getLevel - not resolved
[YuzoraClient] [!!] Actor::getPosition - not resolved
[YuzoraClient] 0 / 4 SDK functions resolved
[YuzoraClient] Production hooks: none registered yet (v0.3 scope: hook foundation only)
[YuzoraClient] Hook diagnostics
[YuzoraClient] 0 / 0 hooks installed
[YuzoraClient] Present vtable hooks installed (... entries, Present1: hooked)
[YuzoraClient] D3D12 ExecuteCommandLists hook installed (original at 0x...)
[YuzoraClient] Render hook installed - overlay should be visible on the game
[YuzoraClient] Initialized
[YuzoraClient] [trace] first intercepted ExecuteCommandLists (queue 0x...)   ← 数秒以内
[YuzoraClient] [trace] overlay D3D12 path active                              ← 数秒以内
[YuzoraClient] [trace] overlay first frame drawn (D3D12)                      ← これが出れば描画成功
```

**重要**:
- `signatures 0/0` と `SDK 0/4` は「シグネチャをまだ登録していない」だけの
  **期待通りの表示**であり、エラーではありません
- Minecraft は D3D12 で描画するため、正常時は `[trace] overlay first frame drawn (D3D12)`
  が出ます。`overlay draw skipped: ...` が出た場合はその理由行を報告してください
- `[warning] initialize() called, but the client is already initialized` は
  初期化を 2 回呼んだときだけ出る注意メッセージで、無害です

### 4-2. ゲーム画面 (確認箇所: Minecraft のウィンドウ左上)

ゲーム画面の**左上**に半透明の黒いボックスと次の文字が出ます:

```
YuzoraClient v0.5.0-dev [game]
Minecraft: 1.21.xxx
XYZ: unavailable          ← シグネチャ未解決のため。仕様通りの表示
FPS: 120
Signatures: 0/0  Hooks: 0/1  SDK: 0/4
```

- `FPS` の数字が毎秒変化すれば Present フックが実働している証拠
- ワールドに入っても表示自体は変わりません (XYZ の実座標化はシグネチャ解決後)

**ここまで確認できたら First Milestone (バージョン検出 → Render Hook → HUD) 達成です。**
DebugView のログ全体とゲーム画面のスクリーンショットを記録しておいてください。

---

## Step 5: 終了する

### 5-1. きれいにシャットダウンする (推奨)

管理者ターミナルで次の 1 行だけ実行 (`Add-Type` 済みの同じターミナルならこれだけで OK):

```powershell
[YuzoraLoader]::Inject("C:\Users\yuzut\OneDrive\デスクトップ\YuzoraClient\build\bin\Release\YuzoraClient.dll", "YuzoraShutdown")
```

DebugView に次が出れば成功:

```
[YuzoraClient] Shutdown
```

シャットダウンするとウォーターマークが消えます (フック解除のため)。
DLL 自体はプロセス内に残りますが、ゲームを閉じる際に一緒に解放されます。

### 5-2. ゲームを閉じる

普通に Minecraft を終了して構いません (シャットダウンを呼ばなくてもプロセス終了で片付きます)。

### 5-3. DLL をビルドし直して再検証するとき

**Minecraft が動いている間は DLL がロックされるため、ビルドし直せません。**

1. Minecraft を**一度完全に終了**する (DLL を差し替えるため)
2. ビルドする
3. Step 1-2 (DebugView 起動) からのやり直し
   - Minecraft の再起動ごとに 1 回の注入で OK
   - **旧 DLL を注入済みの Minecraft にそのまま再注入しても無効です**
     (古い版が残り続けるため)。必ずゲームを再起動してから注入すること

---

## トラブルシュート

| 症状 | 原因と対処 |
|---|---|
| `ERROR: OpenProcess 失敗 (5)` | ターミナルが管理者権限でない。Step 3-1 からやり直す |
| `ERROR: Minecraft が起動していません` | タイトル画面まで起動してから再実行。プロセス名は `Minecraft.Windows` |
| `ERROR: DLL ロード失敗` | DLL パスの誤り、またはビルドが古い。Step 3-2 のパスとファイルの存在を確認 |
| `ERROR: YuzoraInitialize が false を返しました` | ゲームモードの初期化処理で失敗。DebugView の**最後のログ行**が原因箇所 |
| ロードは成功したが何も表示されない | DebugView のログがどこまで出ているか確認。全段階ログ化済みなので最終行が手がかり |
| ウォーターマークが出ない (フックのログはある) | フルスクリーンをやめてウィンドウ/ボーダーレスにする (Step 2-3) |
| ロード直後にゲームがクラッシュ | DebugView の最終ログ + イベントビューアー (Windows ログ → アプリケーション) のエラーを記録して報告する |
| ウイルス対策ソフトがローダーをブロックする | リモートスレッドを使うスクリプトは誤検知されやすい。一時的に許可するか除外する |

---

## 補足

- この手順は**開発者自身の PC・自分が所有するゲームでの検証用**です。サーバー等で使用する場合は各サービスの利用規約を各自で確認してください
- このローダーはリポジトリ外で完結するスクリプトであり、YuzoraClient のコードにはインジェクション機能を含みません
- 検証で得たログ・スクリーンショット・不具合情報は、次のマイルストーン (シグネチャ RE → XYZ 実座標化、v0.6 Module) の資料になります
