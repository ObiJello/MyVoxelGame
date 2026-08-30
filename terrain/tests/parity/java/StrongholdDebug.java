import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.structure.BoundingBox;
import net.minecraft.world.level.levelgen.structure.StructurePiece;
import net.minecraft.world.level.levelgen.structure.pieces.StructurePiecesBuilder;
import net.minecraft.world.level.levelgen.structure.structures.StrongholdPieces;

import java.util.List;

/**
 * Replays StrongholdStructure.generatePieces() with the REAL vanilla
 * StrongholdPieces classes and a draw-logging RandomSource, to produce ground
 * truth for the C++ port. Usage: StrongholdDebug <seed> <cx> <cz> [--log-draws]
 */
public class StrongholdDebug {

    /**
     * MUST extend LegacyRandomSource: WorldgenRandom.next(bits) type-checks
     * `randomSource instanceof LegacyRandomSource` and falls back to
     * top-bits-of-nextLong (Xoroshiro semantics) otherwise - an interface
     * wrapper silently corrupts every draw.
     */
    /**
     * Logs at the WorldgenRandom level so caller-facing nextInt(bound) calls
     * (with their bounds) are captured; the inner source must still be a real
     * LegacyRandomSource for WorldgenRandom.next(bits)'s instanceof check.
     */
    static final class LoggingWorldgenRandom extends WorldgenRandom {
        final boolean log;
        LoggingWorldgenRandom(boolean log) { super(new LegacyRandomSource(0L)); this.log = log; }
        @Override public int nextInt(int bound) {
            int v = super.nextInt(bound);
            if (log) System.out.println("DRAW nextInt(" + bound + ") -> " + v);
            return v;
        }
        @Override public boolean nextBoolean() {
            boolean v = super.nextBoolean();
            if (log) System.out.println("DRAW nextBoolean -> " + v);
            return v;
        }
        @Override public double nextDouble() {
            double v = super.nextDouble();
            if (log) System.out.println("DRAW nextDouble -> " + v);
            return v;
        }
    }

    public static void main(String[] args) throws Exception {
        long seed = Long.parseLong(args[0]);
        int cx = Integer.parseInt(args[1]);
        int cz = Integer.parseInt(args[2]);
        boolean logDraws = args.length > 3 && args[3].equals("--log-draws");

        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        WorldgenRandom random = new LoggingWorldgenRandom(logDraws);
        ChunkPos pos = new ChunkPos(cx, cz);

        int tries = 0;
        StructurePiecesBuilder builder = new StructurePiecesBuilder() {
            @Override public void addPiece(StructurePiece piece) {
                if (logDraws) {
                    BoundingBox bb = piece.getBoundingBox();
                    System.out.println("ADD " + piece.getClass().getSimpleName()
                        + " gd=" + piece.getGenDepth()
                        + " bb=" + bb.minX() + "," + bb.minY() + "," + bb.minZ()
                        + "," + bb.maxX() + "," + bb.maxY() + "," + bb.maxZ());
                }
                super.addPiece(piece);
            }
        };
        StrongholdPieces.StartPiece start;
        do {
            builder.clear();
            random.setLargeFeatureSeed(seed + (long)(tries++), pos.x, pos.z);
            StrongholdPieces.resetPieces();
            start = new StrongholdPieces.StartPiece(random, pos.getBlockX(2), pos.getBlockZ(2));
            builder.addPiece(start);
            start.addChildren(start, builder, random);
            List<StructurePiece> pending = start.pendingChildren;
            while (!pending.isEmpty()) {
                int i = random.nextInt(pending.size());
                StructurePiece piece = pending.remove(i);
                if (logDraws) System.out.println("DRAIN idx=" + i + " -> " + piece.getClass().getSimpleName());
                piece.addChildren(start, builder, random);
            }
            builder.moveBelowSeaLevel(63, -64, random, 10);
            if (logDraws) System.out.println("RETRY done tries=" + tries + " empty=" + builder.isEmpty() + " portal=" + (start.portalRoomPiece != null));
        } while (builder.isEmpty() || start.portalRoomPiece == null);

        System.out.println("TRIES " + tries);
        var container = builder.build();
        for (StructurePiece piece : container.pieces()) {
            BoundingBox bb = piece.getBoundingBox();
            System.out.println("PIECE " + piece.getClass().getSimpleName()
                + " rot=" + (piece.getRotation() == null ? "-" : piece.getRotation().name())
                + " gd=" + piece.getGenDepth()
                + " bb=" + bb.minX() + "," + bb.minY() + "," + bb.minZ()
                + "," + bb.maxX() + "," + bb.maxY() + "," + bb.maxZ());
        }
    }
}
