// Reach a fixed point across direct calls, recovered switch cases and optional callback roots.
//@category BYOG
import ghidra.app.script.GhidraScript;

public class DiscoverStartup extends GhidraScript {
    @Override public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 2) throw new IllegalArgumentException("Expected root file and callback flag");
        boolean callbacks = arguments[1].equals("callbacks");
        for (int pass = 0; pass < 12; ++pass) {
            int beforeFunctions = currentProgram.getFunctionManager().getFunctionCount();
            long beforeInstructions = currentProgram.getListing().getNumInstructions();
            if (callbacks) runScript("SeedCodePointers.java", new String[] {arguments[0]});
            runScript("SeedStartupGroup.java", new String[] {arguments[0]});
            runScript("ResolveJumpTables.java", new String[0]);
            int functions = currentProgram.getFunctionManager().getFunctionCount();
            long instructions = currentProgram.getListing().getNumInstructions();
            println("Startup closure pass " + (pass + 1) + ": " + functions + " functions, " + instructions + " instructions");
            if (functions == beforeFunctions && instructions == beforeInstructions) return;
        }
        throw new IllegalStateException("Startup discovery did not reach a fixed point in 12 passes");
    }
}
