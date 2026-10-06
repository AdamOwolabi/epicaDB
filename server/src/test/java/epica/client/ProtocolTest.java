// ProtocolTest.java -- the Java encoder must produce the exact bytes the C++
// server expects (see engine/net/protocol.h worked example).
package epica.client;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.charset.StandardCharsets;
import java.util.List;
import org.junit.jupiter.api.Test;

class ProtocolTest {
  @Test
  void getFrameMatchesSpec() {
    byte[] payload = Protocol.get("abc".getBytes(StandardCharsets.UTF_8));
    byte[] frame = new Protocol.Writer().u32(payload.length).bytes();
    byte[] expectedPrefix = {8, 0, 0, 0};  // length 8, little-endian
    assertArrayEquals(expectedPrefix, frame);
    byte[] expectedPayload = {0x02, 3, 0, 0, 0, 'a', 'b', 'c'};
    assertArrayEquals(expectedPayload, payload);
  }

  @Test
  void readerDecodesLittleEndian() {
    Protocol.Reader r = new Protocol.Reader(new byte[] {0, 1, 0, 0, 0, 2, 0, 0, 0, 'h', 'i'});
    assertEquals(0, r.u8());
    assertEquals(1, r.u32());
    assertArrayEquals(new byte[] {'h', 'i'}, r.str());
  }

  @Test
  void batchEncoding() {
    byte[] b = Protocol.batch(List.of(BatchOp.put(new byte[] {'a'}, new byte[] {'1'}), BatchOp.delete(new byte[] {'b'})));
    byte[] expected = {0x06, 2, 0, 0, 0, 1, 1, 0, 0, 0, 'a', 1, 0, 0, 0, '1', 0, 1, 0, 0, 0, 'b'};
    assertArrayEquals(expected, b);
  }
}
