# Changelog

**Hapbeat** Arduino ライブラリ（`hapbeat-arduino`）の主要な変更点をまとめます。

## 0.1.0 — 初の公開リリース

Hapbeat 触覚デバイスを Wi-Fi UDP で駆動する level-1 の Arduino / ESP32 / M5Stack
ライブラリ。**依存ゼロ**です（サンプルのみ M5Unified を使用）。

### command（fire）モード
- `play(eventId, gain, target)` / `stop` / `stopAll` / `ping` / `setGroup`。
- kit イベントを id（`<kit-name>.<file-name>`）で発火します。波形は Hapbeat Studio で
  デバイスに導入した kit 側にあり、MCU は約 30 バイトのトリガーを送るだけで何も保存しません。

### 合成サイン波（kit 不要・WAV 不要）
- `playSine(freqHz, intensity, durationMs)` — ワンショット。
- `beginSine` / `pumpSine` / `endSine` / `sineActive` — 連続・オープンエンドな
  ストリーム。押している間の再生や、周波数 / 強度のライブ制御に使えます。
- 16 kHz モノラル PCM16 を MCU 上で生成。約 160 ms の送信先行バッファが Wi-Fi の
  ジッターをデバイス側の約 256 ms リングに対して吸収します。
- 信頼性: `STREAM_BEGIN` / `STREAM_END` は再送します（BEGIN を取りこぼすとストリーム
  全体が無音になるため）。prime / catch-up バーストは `pumpSine()` 呼び出しごとに上限を設けます。

### 検出とアドレッシング
- `discover()` / `deviceIp()` / `setDeviceIp()` — ブロードキャスト PING → PONG で
  デバイス IP を学習し、ストリーミングを **unicast** できます（Wi-Fi MAC の ACK + リトライで
  ブロードキャストよりはるかに滑らか）。デバイスが見つからない場合はブロードキャストに
  フォールバックします。
- `target` アドレッシング: `""`（ブロードキャスト）、`"player_1/chest"`、`"*/chest"`、
  `"group_<N>"`。

### 相互運用
- ワイヤーフォーマットは `hapbeat-contracts` および Python / Unity / JS SDK と
  バイト互換です（`docs/wire-format.md` 参照）。

### 未対応（今後の予定）
- ESP-NOW transport（ルーター不要）、保存 WAV / clip ストリーミング、EventMap による
  調整レイヤー、より高度な合成。
