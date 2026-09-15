/* Who takes ownership of a UHS interface, and who reserves one?
 *
 * The acquire handler grants a request only when iface[0x28] is zero
 * (unowned) and iface[0x2c] is either negative or this client's pid.
 * Failure is silent, so the only way to learn which test we fail is to
 * find every place those two fields are written.
 *
 * Decompiles the whole module and reports the functions that assign to
 * either offset, with the assigning line.
 */
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;

public class FindOwnerWrites extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int scanned = 0, hits = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            scanned++;
            DecompileResults res = decomp.decompileFunction(f, 40, monitor);
            if (res == null || !res.decompileCompleted()) {
                continue;
            }
            String c = res.getDecompiledFunction().getC();
            String[] lines = c.split("\n");
            List<String> found = new ArrayList<>();
            for (String line : lines) {
                String t = line.trim();
                /* an assignment INTO +0x28 or +0x2c, not a read of it */
                if ((t.contains("+ 0x28)") || t.contains("+ 0x2c)")) && t.contains(" = ") &&
                    t.indexOf(" = ") > t.indexOf("0x2")) {
                    found.add(t);
                }
            }
            if (!found.isEmpty()) {
                hits++;
                println("");
                println("### " + f.getName() + " @ " + f.getEntryPoint());
                for (String line : found) {
                    println("    " + line);
                }
            }
        }
        println("");
        println("=== scanned " + scanned + " functions, " + hits + " write to those fields ===");
        decomp.dispose();
    }
}
