/* Finds the UHS client interface state machine and decompiles it.
 *
 * The client manager logs its own transitions -- "Sending probe
 * indication to client in pid %d", "Acquired by client in pid %d" --
 * so those strings are the labels of the very states we are stuck
 * between. This finds them, walks back to whoever references them, and
 * prints the decompilation: the condition that declines to answer an
 * acquire has to live there.
 */
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class FindAcquirePath extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] wanted = { "Enable endpoints" };

        Set<Function> interesting = new LinkedHashSet<>();
        Listing listing = currentProgram.getListing();
        DataIterator data = listing.getDefinedData(true);

        while (data.hasNext() && !monitor.isCancelled()) {
            Data d = data.next();
            Object v = d.getValue();
            if (!(v instanceof String)) {
                continue;
            }
            String s = (String) v;
            boolean hit = false;
            for (String w : wanted) {
                if (s.contains(w)) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                continue;
            }

            println("STRING @ " + d.getAddress() + "  \"" + s.trim() + "\"");
            ReferenceIterator refs =
                currentProgram.getReferenceManager().getReferencesTo(d.getAddress());
            int n = 0;
            while (refs.hasNext()) {
                Reference r = refs.next();
                Function f = getFunctionContaining(r.getFromAddress());
                println("    from " + r.getFromAddress() +
                        (f != null ? ("  in " + f.getName() + " @ " + f.getEntryPoint())
                                   : "  (outside any function)"));
                if (f != null) {
                    interesting.add(f);
                }
                n++;
            }
            if (n == 0) {
                println("    no direct references -- the pointer is built in code");
            }
        }

        println("");
        println("=== " + interesting.size() + " function(s) to read ===");

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (Function f : interesting) {
            println("");
            println("======== " + f.getName() + " @ " + f.getEntryPoint() + " ========");
            DecompileResults res = decomp.decompileFunction(f, 120, monitor);
            if (res != null && res.decompileCompleted()) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("(decompile failed: " +
                        (res != null ? res.getErrorMessage() : "no result") + ")");
            }
        }
        decomp.dispose();
    }
}
