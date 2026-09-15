/* Reports every instruction carrying one of the given immediates, and
 * the function it sits in. The endpoint-slot base offsets (0x5d0 IN,
 * 0x3b4 OUT) pin the code that builds those slots -- which is where an
 * endpoint's permission mask is first written. */
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.util.*;

public class FindImm extends GhidraScript {
    @Override
    public void run() throws Exception {
        Set<Long> want = new HashSet<>();
        for (String a : getScriptArgs()) want.add(Long.parseLong(a, 16));
        Listing l = currentProgram.getListing();
        InstructionIterator it = l.getInstructions(true);
        while (it.hasNext()) {
            Instruction i = it.next();
            boolean hit = false;
            for (int op = 0; op < i.getNumOperands(); op++) {
                for (Object o : i.getOpObjects(op)) {
                    if (o instanceof Scalar && want.contains(((Scalar)o).getUnsignedValue())) hit = true;
                }
            }
            if (!hit) continue;
            Function f = getFunctionContaining(i.getAddress());
            println(i.getAddress() + "  " + i + "   [" + (f==null?"(none)":f.getName()) + "]");
        }
    }
}
