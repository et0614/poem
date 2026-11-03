namespace PoEMLink.Midi
{
  /// <summary>
  /// PoEM Link との SysEx の送受信 (通信仕様の L2')。
  ///
  /// OS ごとの MIDI API (CoreMIDI / Android MIDI / Windows) と、実機なしの模擬
  /// (<see cref="Simulation.SimLinkTransport"/>) がこれを実装する。
  /// SysEx の本体は F0 と F7 を除いたバイト列 (MFR から最後の符号化データまで) で渡す。
  /// 中身の解釈 (SEQ、コマンド、8-to-7 符号化) は <see cref="Link.PoemLinkClient"/> が行う。
  /// </summary>
  public interface ILinkTransport
  {
    /// <summary>SysEx を 1 つ受け取った (F0 と F7 を除いた本体)</summary>
    event Action<byte[]>? SysExReceived;

    /// <summary>Link の接続・切断</summary>
    event Action<bool>? ConnectionChanged;

    bool IsConnected { get; }

    /// <summary>つながっている Link の MIDI ポート名 ("PoEM Link XXXXXXXX")。未接続なら null</summary>
    string? DeviceName { get; }

    /// <summary>SysEx を送る (F0 と F7 は実装が付ける)</summary>
    void SendSysEx(byte[] body);
  }
}
