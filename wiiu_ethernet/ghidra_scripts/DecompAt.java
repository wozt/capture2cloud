/* Decompiles the functions named in ADDRESSES. Used to follow a call
 * chain without re-running the whole analysis each time. */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] addresses = { "101147b0", "10115c2c" };
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (String a : addresses) {
            Address addr = currentProgram.getAddressFactory().getAddress("0x" + a);
            Function f = getFunctionAt(addr);
            if (f == null) {
                f = getFunctionContaining(addr);
            }
            println("======== " + a + " " + (f == null ? "(no function)" : f.getName()) +
                    " ========");
            if (f == null) {
                continue;
            }
            DecompileResults res = decomp.decompileFunction(f, 120, monitor);
            if (res != null && res.decompileCompleted()) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("(failed)");
            }
        }
        decomp.dispose();
    }
}
