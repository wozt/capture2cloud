/* Walks up from a known ioctl handler to the function that dispatches on
 * the request number. A call in ARM is a relative BL, so the handler's
 * address never appears as a literal in the image -- only Ghidra's
 * reference model can answer "who calls this". */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.*;

public class FindDispatch extends GhidraScript {

    private List<Function> callersOf(Address a) {
        List<Function> out = new ArrayList<>();
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
        while (it.hasNext()) {
            Reference r = it.next();
            Function f = getFunctionContaining(r.getFromAddress());
            println("    ref from " + r.getFromAddress() + " (" + r.getReferenceType() + ") in "
                    + (f == null ? "(none)" : f.getName() + " @ " + f.getEntryPoint()));
            if (f != null && !out.contains(f)) {
                out.add(f);
            }
        }
        return out;
    }

    @Override
    public void run() throws Exception {
        String[] seeds = { "10115dac", "10115c2c", "10114238", "101147b0" };
        Set<String> seen = new HashSet<>();
        List<Function> frontier = new ArrayList<>();

        for (String s : seeds) {
            Address a = currentProgram.getAddressFactory().getAddress("0x" + s);
            Function f = getFunctionAt(a);
            println("== callers of " + s + " (" + (f == null ? "no function" : f.getName()) + ")");
            for (Function c : callersOf(a)) {
                if (seen.add(c.getEntryPoint().toString())) {
                    frontier.add(c);
                }
            }
        }

        /* One more level up: the dispatch is usually the caller's caller. */
        println("\n== one level up");
        List<Function> up = new ArrayList<>();
        for (Function f : new ArrayList<>(frontier)) {
            println("  from " + f.getName() + " @ " + f.getEntryPoint());
            for (Function c : callersOf(f.getEntryPoint())) {
                if (seen.add(c.getEntryPoint().toString())) {
                    up.add(c);
                }
            }
        }
        frontier.addAll(up);

        println("\n== candidate set");
        for (Function f : frontier) {
            println("  " + f.getName() + " @ " + f.getEntryPoint() + "  body="
                    + f.getBody().getNumAddresses() + " bytes");
        }
    }
}
