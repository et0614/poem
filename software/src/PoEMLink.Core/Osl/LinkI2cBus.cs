using PoEMLink.Link;

namespace PoEMLink.Osl
{
  /// <summary>PoEM Link を OSL 層のバスとして使う</summary>
  public sealed class LinkI2cBus : II2cBus
  {
    private readonly PoemLinkClient _link;
    private readonly int _maxBatch;
    private readonly bool _hasBatch;

    public int MaxTransfer { get; }

    /// <param name="info">GET_INFO の応答。上限値と BATCH の有無に使う (null なら仕様の既定値)</param>
    public LinkI2cBus(PoemLinkClient link, LinkInfo? info)
    {
      _link = link;
      MaxTransfer = info?.MaxTransfer ?? 256;
      _maxBatch = info?.MaxBatch ?? 512;
      _hasBatch = info == null || info.Features.HasFlag(LinkFeatures.Batch);
    }

    public Task<IReadOnlyList<byte>> ScanAsync(byte first, byte last, CancellationToken ct = default)
      => _link.ScanAsync(first, last, ct);

    public async Task<byte[]?> ReadAsync(byte addr, int n, CancellationToken ct = default)
    {
      var r = await _link.ReadAsync(addr, n, ct).ConfigureAwait(false);
      return r.IsOk && r.Data.Length == n ? r.Data : null;
    }

    public async Task<bool> WriteAsync(byte addr, ReadOnlyMemory<byte> data, CancellationToken ct = default)
    {
      var r = await _link.WriteAsync(addr, data, ct).ConfigureAwait(false);
      return r.IsOk;
    }

    public async Task<byte[]?> WriteReadAsync(byte addr, ReadOnlyMemory<byte> write, int n, CancellationToken ct = default)
    {
      var r = await _link.WriteReadAsync(addr, write, n, ct).ConfigureAwait(false);
      return r.IsOk && r.Data.Length == n ? r.Data : null;
    }

    public async Task<IReadOnlyList<byte[]?>> WriteReadManyAsync(IReadOnlyList<(byte Addr, byte[] Write, int ReadLength)> requests, CancellationToken ct = default)
    {
      var results = new byte[]?[requests.Count];
      if (!_hasBatch)
      {
        for (int i = 0; i < requests.Count; i++)
          results[i] = await WriteReadAsync(requests[i].Addr, requests[i].Write, requests[i].ReadLength, ct).ConfigureAwait(false);
        return results;
      }

      // 応答のデータが MAX_BATCH に、操作の数が 64 に収まるように分ける
      int start = 0;
      while (start < requests.Count)
      {
        var ops = new List<BatchOp>();
        int size = 0;
        for (int i = start; i < requests.Count && ops.Count < 64; i++)
        {
          var op = BatchOp.WriteRead(requests[i].Addr, requests[i].Write, requests[i].ReadLength);
          if (ops.Count > 0 && size + op.ResponseSize > _maxBatch) break;
          ops.Add(op);
          size += op.ResponseSize;
        }

        var rs = await _link.BatchAsync(ops, stopOnError: false, ct).ConfigureAwait(false);
        for (int j = 0; j < ops.Count; j++)
        {
          bool ok = j < rs.Count && rs[j].IsOk && rs[j].Data.Length == ops[j].ReadLength;
          results[start + j] = ok ? rs[j].Data : null;
        }
        start += ops.Count;
      }
      return results;
    }
  }
}
