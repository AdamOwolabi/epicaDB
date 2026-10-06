// BatchOp.java -- one operation inside an atomic batch (put or delete).
package epica.client;

public record BatchOp(boolean isPut, byte[] key, byte[] value) {
  public static BatchOp put(byte[] k, byte[] v) { return new BatchOp(true, k, v); }
  public static BatchOp delete(byte[] k) { return new BatchOp(false, k, new byte[0]); }
}
