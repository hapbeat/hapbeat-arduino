# Changelog

**Hapbeat** Arduino ライブラリ（`hapbeat-arduino`）の主要な変更点をまとめます。

## 1.0.0 — Stream Gen 2（1.x の通信）に対応

対応する contracts: 1.0.0（tag `v1.0.0`）。

- **Stream Gen 2**（セッション方式、contracts `stream-session.md`、DEC-096）に対応しました。
  ファーム 0.5.0 以降の機体には Stream Gen 2 で、0.4.x 以前の機体には Stream Gen 1
  で送ります。`tick()` が、source の target に一致する機体へ 16 byte の拡張 PING を
  unicast で送り（broadcast しない。`ping()` / `discover()` は 8 byte のまま）、その
  直接応答の PONG の tail で機体ごとに形式を決めます。照合しない PONG は形式を変えません。
  - Stream Gen 2: header version 2 と 16 byte の envelope（boot・ticket・generation）、
    機体が発行する lease（5 s ごとに更新、最後の session の後 15 s まで）、END 直後の
    次の世代（300 ms 待たない）、BEGIN の再送（最初の 3 つの DATA の前と 500 ms 以下ごと）、
    `tick()` が 3 s 呼ばれなかったときの次の世代での再開、同じ boot の ticket 変更で
    cursor を引き継ぐ再開。
  - Stream Gen 1 の機体だけ、END→BEGIN の 310 ms guard を残しました（待つ間 source は
    `Active`）。
  - ERROR（0xFF）を受けます。既知の機体の port 7700 からのものだけ、stream のコードは
    自分の session の envelope と一致するものだけを扱います。`STREAM_NOT_STARTED`
    で BEGIN を再送、`STREAM_SUPERSEDED` で保留、`STREAM_LEASE_INVALID` で lease を
    取り直し、`STREAM_GEN_UNSUPPORTED` で分類し直します。`STREAM_STOPPED`（ほかの送り手の
    STOP_ALL）では、その session の source をその機体から外し、同じ Playback では戻しません
    （新しい Playback はふつうに参加します。`sdk-multi-stream.md` §6.13）。
  - `stopAll()` は stream の session をその場で終えるようにしました。前は session を
    310 ms 残したので、直後に始めた source が、機体が STOP_ALL で止めた session に
    DATA を送り続け、鳴りませんでした（0.2.0 の Stream Gen 1 でも同じでした）。
  - 最後に lease を受け取ってから 14 s を過ぎた機体には、新しい session を始める前に lease を取り直します（機体は使っていない lease を 15 s で捨てるので、古い lease の BEGIN は `STREAM_LEASE_INVALID` で断られていました）。
  - 長さのある source は、どこか 1 台で最後まで鳴らし、どの session にも残っていなければ
    `Stopped` になります。使えるようにならなかった機体は待ちません。前は、一致する機体の
    1 台が始められないと、鳴らし終えた機体で頭から鳴らし直し続けていました。
- 追加: `HapbeatDeferredReason` の `UnknownClass` / `LeaseUnavailable` / `Superseded`、
  `HapbeatStreamClass`、`HapbeatLeaseState`、`streamClass(ip)`、
  `streamEndpoints(out, max)`、`reacquireStreamOwnership()`、
  `setWarningHandler(fn)`（ライブラリは Serial に出力しません）。
- 含まないもの: `STREAM_TIMED_OUT` / `STREAM_TARGET_MISMATCH` の
  ERROR への動作（警告だけ）、CAPS の読み取り（新しい major の機体への警告、モーター 1 個の
  機体で pan を掛けない混合）、endpoint の変化の callback、gain・pan の平滑化・limiter・
  差し替え（`sdk-multi-stream.md` §5.7・§5.8・§6.14）。ESP32 / ESP8266 以外のボードの
  client incarnation は `random()` による弱い乱数です。
- `playSineSource(...)` と `tick()` を追加し、固定容量の source / endpoint session
  hub で複数サイン波を同時に扱えるようにしました。各 PONG の完全アドレスへ
  16 kHz stereo PCM16 を exact unicast し、未解決時は `Deferred`、空 session は
  310 ms linger、Stream Gen 1 の機体では `STREAM_END` 後に 310 ms cooldown になります。旧 single sine API
  (`beginSine` / `pumpSine` / `endSine` / `sineActive`) は削除しました。
- `play()` の `gain` を、PLAY に `clamp(gain, 0, 1)` で載せるようにしました（NaN・無限は 0）。本体は gain を切らないので、前は 1 を超えると歪み、負だと波形が反転していました。このライブラリは Kit の manifest を読まないので、Kit の `clip_gain` は掛けません（1 とみなす。contracts `kit-format.md`「強さの式（正本）」、DEC-103）。
- `play()` に `pan` 引数（左右バランス。-1 = 左のみ / 0 = 中央 / +1 = 右のみ、既定 0）を追加し、PLAY パケット末尾に float32 として常に送るようにしました（contracts DEC-055）。

## 0.2.0 — 複数デバイス追跡とコマンドのユニキャスト化

### 変更（既定の挙動）
- **PLAY / STOP / STOP_ALL も、PONG を返した既知デバイスへユニキャスト送信**する
  ようになりました（従来はブロードキャスト。ストリームだけが unicast でした）。
  Wi-Fi AP は、同じ AP に省電力状態の端末が 1 台でもいるとブロードキャストフレームを
  次の DTIM ビーコン（100〜300 ms 周期）まで保留するため、その 1 発が最大 300 ms
  遅れていました。ユニキャストはこの保留を受けません。
  - 既知デバイスが 0 台のときはブロードキャストにフォールバックします。
  - **ユニキャストとブロードキャストの二重送信はしません。**
  - PING はデバイス検出のためブロードキャストのままです。
- **デバイステーブルを複数台対応にしました**（従来は `_deviceIp` 1 台のみ）。最大 8 台を
  固定長配列で保持します（MCU なので動的確保はしません）。これが無いままコマンドを
  ユニキャスト化すると、**複数台構成で 1 台にしか届かなくなる**ため、セットでの変更です。
  ストリーム送出も同じテーブルを使うようになり、従来「最初に見つかった 1 台」にしか
  流れていなかったのが解消されました。
- **`discover()` はタイムアウトまで待つようになりました**（従来は最初の 1 台で即 return）。
  応答した全デバイスを宛先に登録するためです。戻り値の意味は変わりません
  （`true` = 1 台以上見つかった）。
- 最後の PONG から一定時間（既定 15 秒）応答が無いデバイスは宛先から外れます。
  外さないと電源 OFF の相手へ送り続け、かつ「生存 0 台でもブロードキャストに戻れない」
  状態になるためです。**`loop()` から数秒おきに `ping()` を呼ぶ**とユニキャストが維持されます。

### 追加
- `poll()` — 届いている PONG を非ブロッキングで取り込む（送信時にも自動で行うため任意）。
- `deviceCount()` — 生存している既知デバイス数。
- `setDeviceTimeout(ms)` — 生存判定の窓（既定 15000）。
- `setBroadcastOnly(bool)` — 常にブロードキャストで送る。多数台を厳密に同時発火させたい
  用途では、1 回の送信で全台に届くブロードキャストの方が有利です（ユニキャストは台数分を
  順に送るため、20 台で 3〜8 ms 程度の時間差が出ます）。
- `setDeviceIp(ip)` は「固定の宛先」を意味するようになりました（生存判定で消えません）。
  `0.0.0.0` を渡すと固定を解除します。

### 修正（実機検証で判明したもの）
- **PONG の受信が最初の 1 通で永久に止まる問題**を修正しました。ESP32 の
  `WiFiUDP::parsePacket()` は前のデータグラムを読み切っていないと次を一切読まない
  ため、ヘッダだけ読んで残りを捨てていた `pollPongs()` で受信が固まっていました。
  0.1.0 では `discover()` が最初の応答で即 return し以後ポーリングしなかったため
  表面化していませんでしたが、継続ポーリング前提の本版では「ユニキャストが 15 秒で
  無言のうちにブロードキャストへ降格する」退行になります。
- **経路を切り替えた直後にストリームが止まる問題**を修正しました。`endSine()` の
  `STREAM_END` は切替前の経路、直後の `beginSine()` の `STREAM_BEGIN` は切替後の
  経路で出ます。ブロードキャストの END は AP の DTIM バッファに最大 300 ms 保留
  される一方ユニキャストの BEGIN は即着するため、デバイスには BEGIN → 遅れて END
  の順で届き、開始直後のストリームが停止していました。`beginSine()` は再アームを
  許可したので、**再開のために `endSine()` → `beginSine()` を続けて呼ばないでください**
  （経路を変えるだけならどちらも不要です）。
- **`beginSine()` が狙いのバッファまで満たしてから戻る**ようになりました。従来は
  約 64 ms 分だけ送って戻っていたため、呼び出し側が直後に画面描画などで数十 ms
  ブロックするとデバイスのリングが尽き、開始直後に一瞬途切れていました。
- **`end()`** が再生中でも `STREAM_END` を送らずに終了していたのを修正しました
  （デバイスが止められなくなる）。
- **`stopAll()`** がローカルのストリームを止めていなかったのを修正しました。
  Python / JS SDK の `stop_all()` / `stopAll()` と挙動を揃えています。
- 到達しない宛先への送信が連続 5 回失敗したらテーブルから外すようにしました
  （PONG を受ければ入り直ります）。
- ユニキャスト時は送出パケット数が宛先数だけ増えるため、1 回あたりのチャンク数を
  宛先数で割るようにしました（2 台で lwIP の TX プールが枯渇していました）。
- `ping()` がブロードキャストに加えて既知デバイスへユニキャストでも送るように
  なりました。生存確認が二重化され、その host のアドレス解決も維持されます。

### 追加（続き）
- `deviceIps(IPAddress* out, uint8_t maxOut)` — 現在のユニキャスト宛先一覧。

### 注意（0.1.0 の記述の訂正）
- **`"group_<N>"` 単独のターゲットは機能しません。** 照合は位置ベースで、`group_2` は
  player スロットと比較されるため一致しません。group だけを指定したい場合は
  **`"*/*/group_2"`** のように前のスロットを `*` で埋めてください。
- **部分ワイルドカードは使えません。** `*` はセグメント全体が `*` のときだけ有効です
  （`player_1/pos_*` は不一致、`player_1/*` は一致）。
- デバイスのアドレスは常に `player_<N>/<position>/group_<M>` の正規形で、既定は
  `player_1` / `group_1` です。

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
