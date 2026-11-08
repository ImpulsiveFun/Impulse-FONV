// Discover aligned MSVC callback entries at padding or decoded return boundaries.
//@category BYOG
import java.util.TreeSet;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;

public class SeedCodePointers extends GhidraScript {
    @Override public void run() throws Exception {
        TreeSet<Address> candidates = new TreeSet<>();
        var memory = currentProgram.getMemory();
        for (Instruction instruction : currentProgram.getListing().getInstructions(true)) {
            String mnemonic = instruction.getMnemonicString();
            if (!mnemonic.equals("PUSH") && !mnemonic.equals("MOV")) continue;
            int operand = instruction.getNumOperands() - 1;
            if (operand < 0) continue;
            int type = instruction.getOperandType(operand);
            if ((type & OperandType.SCALAR) == 0 || (type & OperandType.DYNAMIC) != 0) continue;
            Scalar scalar = instruction.getScalar(operand);
            if (scalar == null) continue;
            long value = scalar.getUnsignedValue();
            if (value == 0 || value > 0xffffffffL) continue;
            Address address = toAddr(value);
            MemoryBlock block = memory.getBlock(address);
            if (block == null || !block.isExecute() || address.equals(block.getStart())) continue;
            if (memory.getByte(address) == (byte)0xcc) continue;
            boolean hotpatch = address.add(1).compareTo(block.getEnd()) <= 0 &&
                memory.getByte(address) == (byte)0x8b && memory.getByte(address.add(1)) == (byte)0xff;
            if ((value & 15) != 0 && !hotpatch) continue;
            boolean boundary = hotpatch || memory.getByte(address.subtract(1)) == (byte)0xcc;
            Instruction previous = currentProgram.getListing().getInstructionBefore(address);
            if (previous != null && previous.getMaxAddress().add(1).equals(address) &&
                (previous.getMnemonicString().startsWith("RET") || (previous.getFlowType().isJump() && !previous.getFlowType().isConditional()))) boundary = true;
            if (!boundary) continue;
            if (getFunctionAt(address) == null) candidates.add(address);
        }
        int dataCandidates = 0;
        // Engine dispatch tables often have no RTTI and never materialize their
        // entries as instruction immediates. Require a padded entry boundary
        // and another executable pointer nearby before treating data as code.
        for (MemoryBlock data : memory.getBlocks()) {
            if ((!data.getName().equals(".rdata") && !data.getName().equals(".data")) || !data.isInitialized()) continue;
            byte[] chunk = new byte[1024 * 1024 + 16];
            for (long offset = 0; offset < data.getSize(); offset += 1024 * 1024) {
                int length = (int)Math.min(chunk.length, data.getSize() - offset);
                memory.getBytes(data.getStart().add(offset), chunk, 0, length);
                int limit = (int)Math.min(1024 * 1024, data.getSize() - offset);
                for (int i = 0; i + 4 <= limit; i += 4) {
                    long value = u32(chunk, i);
                    Address entry = tableEntry(value);
                    boolean named = false;
                    if (entry == null && value != 0 && (value & 15) == 0) {
                        Address target=toAddr(value);MemoryBlock code=memory.getBlock(target);
                        if(code!=null&&code.isExecute()&&memory.getByte(target)!=(byte)0xcc) {
                            named=(i>=4&&namedMetadata(u32(chunk,i-4)))||(i>=8&&namedMetadata(u32(chunk,i-8)));
                            if(named)entry=target;
                        }
                    }
                    if (entry == null || getFunctionAt(entry) != null) continue;
                    // Havok allocation factories can be singleton pointers among numeric
                    // defaults. Their first call resolves the thread-local allocator.
                    boolean allocatorFactory = false;
                    if (memory.getByte(entry) == (byte)0xe8) {
                        long target=(entry.getOffset()+5+(long)memory.getInt(entry.add(1)))&0xffffffffL;
                        allocatorFactory=target==0xc85750L;
                    }
                    boolean neighbor = named || allocatorFactory;
                    for (int delta : new int[] {-8, -4, 4, 8}) {
                        if (i + delta < 0 || i + delta + 4 > length) continue;
                        long other = u32(chunk, i + delta);
                        Address nearby = toAddr(other);
                        MemoryBlock block = memory.getBlock(nearby);
                        if (block != null && block.isExecute() &&
                            (getFunctionAt(nearby) != null || tableEntry(other) != null)) { neighbor = true; break; }
                    }
                    if (!neighbor && i >= 4 && namedMetadata(u32(chunk, i - 4))) neighbor = true;
                    if (!neighbor && i >= 8 && namedMetadata(u32(chunk, i - 8))) neighbor = true;
                    if (neighbor && candidates.add(entry)) ++dataCandidates;
                }
            }
        }
        int created = 0;
        for (Address address : candidates) {
            if (monitor.isCancelled()) break;
            if (disassemble(address) && createFunction(address, null) != null) ++created;
        }
        println("Code-pointer roots: " + candidates.size() + " candidates (" + dataCandidates + " from data tables), " + created + " created");
    }
    private long u32(byte[] bytes, int offset) {
        return (bytes[offset] & 255L) | ((bytes[offset + 1] & 255L) << 8) |
            ((bytes[offset + 2] & 255L) << 16) | ((bytes[offset + 3] & 255L) << 24);
    }
    private Address paddedEntry(long value) throws Exception {
        if (value == 0 || value > 0xffffffffL) return null;
        Address address = toAddr(value);
        var memory = currentProgram.getMemory();
        MemoryBlock block = memory.getBlock(address);
        if (block == null || !block.isExecute() || address.equals(block.getStart()) || address.equals(block.getEnd())) return null;
        int opcode = memory.getByte(address) & 255;
        if (opcode == 0xcc) return null;
        boolean hotpatch = opcode == 0x8b && memory.getByte(address.add(1)) == (byte)0xff;
        boolean boundary = hotpatch || memory.getByte(address.subtract(1)) == (byte)0xcc;
        Instruction previous = currentProgram.getListing().getInstructionBefore(address);
        if (previous != null && previous.getMaxAddress().add(1).equals(address) &&
            (previous.getMnemonicString().startsWith("RET") || (previous.getFlowType().isJump() && !previous.getFlowType().isConditional()))) boundary = true;
        if (!boundary) return null;
        // Unaligned data words need a conventional entry instruction as well.
        if ((value & 15) != 0 && !hotpatch && opcode != 0x55 && opcode != 0x56 && opcode != 0x57 &&
            opcode != 0x53 && opcode != 0x8b && opcode != 0x33 && opcode != 0xb8 && opcode != 0xa1 &&
            opcode != 0x6a && opcode != 0x68 && opcode != 0xe9 && opcode != 0xeb && opcode != 0xff &&
            opcode != 0xc3 && opcode != 0xc2 && opcode != 0x83) return null;
        return address;
    }

    private Address tableEntry(long value) throws Exception {
        Address padded = paddedEntry(value);
        if (padded != null) return padded;
        // Switch-table bytes can end directly at the next aligned function.
        // In an executable-pointer table, require a recognizable MSVC prologue.
        if ((value & 15) != 0 || value == 0 || value > 0xffffffffL) return null;
        Address address = toAddr(value);
        var memory = currentProgram.getMemory();
        MemoryBlock block = memory.getBlock(address);
        if (block == null || !block.isExecute() || address.add(2).compareTo(block.getEnd()) > 0) return null;
        int a=memory.getByte(address)&255,b=memory.getByte(address.add(1))&255,c=memory.getByte(address.add(2))&255;
        boolean prologue=(a==0x55&&b==0x8b&&c==0xec) || (a==0x6a&&b==0xff&&c==0x68) ||
            (a==0x53&&(b==0x56||b==0x57||b==0x8b)) || (a==0x56&&(b==0x57||b==0x8b)) ||
            (a==0x57&&b==0x8b) || ((a==0x83||a==0x81)&&b==0xec) || a==0x8b || a==0xa1 || (a==0xff&&b==0x25);
        return prologue?address:null;
    }

    private boolean namedMetadata(long value) throws Exception {
        if (value == 0 || value > 0xffffffffL) return false;
        Address address=toAddr(value);var memory=currentProgram.getMemory();
        MemoryBlock block=memory.getBlock(address);
        if(block==null||block.isExecute()||!block.isInitialized())return false;
        boolean letter=false;
        for(int i=0;i<128&&address.add(i).compareTo(block.getEnd())<=0;++i){
            int ch=memory.getByte(address.add(i))&255;
            if(ch==0)return i>=3&&letter;
            if((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z'))letter=true;
            else if(!((ch>='0'&&ch<='9')||ch=='_'||ch=='$'||ch==':'||ch=='.'||ch=='?'||ch=='@'))return false;
        }
        return false;
    }

}
