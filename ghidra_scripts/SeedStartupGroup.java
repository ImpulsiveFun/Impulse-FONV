// Discover a bounded group of directly called startup functions.
//@category BYOG
import java.util.ArrayDeque;
import java.util.HashSet;
import java.nio.file.Files;
import java.nio.file.Path;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

public class SeedStartupGroup extends GhidraScript {
    @Override
    public void run() throws Exception {
        ArrayDeque<Address> pending = new ArrayDeque<>();
        HashSet<Address> seen = new HashSet<>();
        for (String value : getScriptArgs()) {
            if (value.startsWith("@")) {
                for (String entry : Files.readAllLines(Path.of(value.substring(1))))
                    pending.add(toAddr(Long.parseUnsignedLong(entry, 16)));
            } else pending.add(toAddr(Long.parseUnsignedLong(value, 16)));
        }
        // Jump-table resolution disassembles case blocks which can lie outside
        // Ghidra's original function body. Include their direct calls as roots.
        for (Instruction instruction : currentProgram.getListing().getInstructions(true)) {
            if (!instruction.getFlowType().isCall()) continue;
            for (Address target : instruction.getFlows()) pending.add(target);
        }
        while (!pending.isEmpty() && seen.size() < 131072 && !monitor.isCancelled()) {
            Address address = pending.remove();
            if (!seen.add(address)) continue;
            MemoryBlock block = currentProgram.getMemory().getBlock(address);
            if (block == null || !block.isExecute()) continue;
            Function function = getFunctionAt(address);
            if (function == null) {
                if (!disassemble(address)) continue;
                function = createFunction(address, null);
            }
            if (function == null || function.isExternal()) continue;
            for (Instruction instruction : currentProgram.getListing().getInstructions(function.getBody(), true)) {
                if (!instruction.getFlowType().isCall()) continue;
                for (Address target : instruction.getFlows()) pending.add(target);
            }
        }
        println("Startup group: visited " + seen.size() + " entries; pending " + pending.size());
        if (!pending.isEmpty()) throw new IllegalStateException("Startup discovery did not exhaust its queue.");
    }
}
