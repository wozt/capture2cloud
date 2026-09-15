/* Which function loads a given literal address? Used to find the
 * handler behind a log line when the string itself is a format with a
 * %s verb, so searching for the verb finds nothing. */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class WhoUses extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address target = currentProgram.getAddressFactory().getAddress("0x10140b18");
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(target);
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        int n = 0;
        while (refs.hasNext()) {
            Reference r = refs.next();
            Function f = getFunctionContaining(r.getFromAddress());
            println("ref from " + r.getFromAddress() + " in " +
                    (f == null ? "(none)" : f.getName() + " @ " + f.getEntryPoint()));
            if (f != null && n++ < 3) {
                DecompileResults res = decomp.decompileFunction(f, 120, monitor);
                if (res != null && res.decompileCompleted()) {
                    println(res.getDecompiledFunction().getC());
                }
            }
        }
        if (n == 0) {
            println("no references -- the pool entry is not marked as one");
        }
        decomp.dispose();
    }
}
