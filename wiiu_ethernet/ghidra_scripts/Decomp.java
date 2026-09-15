/* Decompiles whatever addresses are given as script arguments. */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (String a : getScriptArgs()) {
            Address addr = currentProgram.getAddressFactory().getAddress("0x" + a);
            Function f = getFunctionAt(addr);
            if (f == null) f = getFunctionContaining(addr);
            println("======== " + a + " " + (f == null ? "(no function)" : f.getName()) + " ========");
            if (f == null) continue;
            DecompileResults r = d.decompileFunction(f, 180, monitor);
            if (r != null && r.decompileCompleted()) println(r.getDecompiledFunction().getC());
            else println("(failed: " + (r == null ? "null" : r.getErrorMessage()) + ")");
        }
        d.dispose();
    }
}
