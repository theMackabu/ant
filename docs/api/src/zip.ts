import { HttpError } from './errors';

export async function readZipText(zip: ArrayBuffer, wantedName: string): Promise<string> {
  const fileBytes = await readZipEntry(zip, wantedName);
  return new TextDecoder().decode(fileBytes);
}

export async function readZipEntry(zip: ArrayBuffer, wantedName: string): Promise<Uint8Array> {
  const bytes = new Uint8Array(zip);
  const view = new DataView(zip);
  const eocd = findEndOfCentralDirectory(view);
  const entries = view.getUint16(eocd + 10, true);
  let cursor = view.getUint32(eocd + 16, true);

  for (let i = 0; i < entries; i++) {
    if (view.getUint32(cursor, true) !== 0x02014b50) {
      throw new HttpError('invalid zip central directory', 502);
    }

    const method = view.getUint16(cursor + 10, true);
    const compressedSize = view.getUint32(cursor + 20, true);
    const filenameLength = view.getUint16(cursor + 28, true);
    const extraLength = view.getUint16(cursor + 30, true);
    const commentLength = view.getUint16(cursor + 32, true);
    const localOffset = view.getUint32(cursor + 42, true);
    const name = new TextDecoder().decode(bytes.slice(cursor + 46, cursor + 46 + filenameLength));

    if (name === wantedName || name.endsWith(`/${wantedName}`)) {
      if (view.getUint32(localOffset, true) !== 0x04034b50) {
        throw new HttpError('invalid zip local header', 502);
      }
      const localNameLength = view.getUint16(localOffset + 26, true);
      const localExtraLength = view.getUint16(localOffset + 28, true);
      const dataStart = localOffset + 30 + localNameLength + localExtraLength;
      const compressed = bytes.slice(dataStart, dataStart + compressedSize);
      return decompressZipEntry(compressed, method);
    }

    cursor += 46 + filenameLength + extraLength + commentLength;
  }

  throw new HttpError(`zip entry not found: ${wantedName}`, 502);
}

async function decompressZipEntry(bytes: Uint8Array, method: number): Promise<Uint8Array> {
  if (method === 0) return bytes;
  if (method !== 8) throw new HttpError(`unsupported zip compression method: ${method}`, 502);

  const input = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(bytes);
      controller.close();
    },
  });
  const stream = (input as ReadableStream<BufferSource>).pipeThrough(
    new DecompressionStream('deflate-raw'),
  );
  const buffer = await new Response(stream).arrayBuffer();
  return new Uint8Array(buffer);
}

function findEndOfCentralDirectory(view: DataView): number {
  const min = Math.max(0, view.byteLength - 0xffff - 22);
  for (let cursor = view.byteLength - 22; cursor >= min; cursor--) {
    if (view.getUint32(cursor, true) === 0x06054b50) return cursor;
  }
  throw new HttpError('invalid zip: missing end of central directory', 502);
}
