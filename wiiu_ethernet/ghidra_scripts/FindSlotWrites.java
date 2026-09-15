/* Finds every store to [reg + 0x14] and [reg + 0x18] -- the two fields
 * the endpoint permission check reads -- and names the function each is
 * in. Whoever grants an endpoint to a client writes those fields; if the
 * only writer is enumeration code, usermode cannot reach it. */
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.util.*;

public class FindSlotWrites extends GhidraScript {
    @Override
    public void run() throws Exception {
        Set<String> want = new HashSet<>(Arrays.asList("0x14", "0x18"));
        Listing l = currentProgram.getListing();
        InstructionIterator it = l.getInstructions(true);
        Map<String,Integer> byFunc = new TreeMap<>();
        while (it.hasNext()) {
            Instruction i = it.next();
            String m = i.getMnemonicString();
            if (!m.startsWith("str")) continue;
            String s = i.toString();
            /* look for a "#0x14]" or "#0x18]" displacement */
            if (!(s.contains(",#0x14]") || s.contains(",#0x18]"))) continue;
            Function f = getFunctionContaining(i.getAddress());
            String key = (f == null ? "(none)" : f.getName() + " @ " + f.getEntryPoint());
            byFunc.merge(key, 1, Integer::sum);
            println(i.getAddress() + "  " + s + "   [" + key + "]");
        }
        println("\n== by function");
        for (Map.Entry<String,Integer> e : byFunc.entrySet())
            println("  " + e.getValue() + "x  " + e.getKey());
    }
}
