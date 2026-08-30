import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.level.ChunkLevel;
import net.minecraft.world.level.chunk.status.ChunkPyramid;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.chunk.status.ChunkStep;

public class PyramidDump {
    public static void main(String[] args) {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        System.out.println("byStatus(FEATURES) = " + ChunkLevel.byStatus(ChunkStatus.FEATURES));
        System.out.println("byStatus(FULL) = " + ChunkLevel.byStatus(ChunkStatus.FULL));
        ChunkStep fullStep = ChunkPyramid.GENERATION_PYRAMID.getStepTo(ChunkStatus.FULL);
        System.out.print("FULL accumulated: ");
        for (int i = 0; i < fullStep.accumulatedDependencies().size(); i++) {
            System.out.print(i + "=" + fullStep.accumulatedDependencies().get(i) + " ");
        }
        System.out.println();
        ChunkStep featStep = ChunkPyramid.GENERATION_PYRAMID.getStepTo(ChunkStatus.FEATURES);
        System.out.print("FEATURES accumulated: ");
        for (int i = 0; i < featStep.accumulatedDependencies().size(); i++) {
            System.out.print(i + "=" + featStep.accumulatedDependencies().get(i) + " ");
        }
        System.out.println();
        for (int lvl = 33; lvl <= 45; lvl++) {
            System.out.println("generationStatus(" + lvl + ") = " + ChunkLevel.generationStatus(lvl));
        }
    }
}
