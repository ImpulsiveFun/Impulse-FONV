// Disassemble the narrow set of padded constant-return entries selected by prepare.py.
//@category BYOG

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

public class SeedProbeFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        List<String> entries = new ArrayList<>();
        for (String value : getScriptArgs()) {
            if (value.startsWith("@")) entries.addAll(Files.readAllLines(Path.of(value.substring(1))));
            else entries.add(value);
        }
        for (String entry : entries) {
            Address address = toAddr(Long.parseUnsignedLong(entry, 16));
            if (!disassemble(address) || createFunction(address, null) == null) {
                throw new IllegalStateException("Could not create probe function at " + address);
            }
        }
    }
}
