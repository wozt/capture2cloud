/* Prints the instructions in an address range, and names any function
 * each call target belongs to. Used where Ghidra disassembled code but
 * did not form a function around it -- a dispatch table reached only by
 * a computed jump looks exactly like that. */
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;

public class DumpRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long start = Long.parseLong(args.length > 0 ? args[0] : "10111400", 16);
        long end   = Long.parseLong(args.length > 1 ? args[1] : "10111700", 16);

        Address a = currentProgram.getAddressFactory().getAddress("0x" + Long.toHexString(start));
        Address z = currentProgram.getAddressFactory().getAddress("0x" + Long.toHexString(end));

        Listing l = currentProgram.getListing();
        InstructionIterator it = l.getInstructions(a, true);
        while (it.hasNext()) {
            Instruction i = it.next();
            if (i.getAddress().compareTo(z) > 0) break;
            StringBuilder sb = new StringBuilder();
            sb.append(i.getAddress()).append("  ").append(i.toString());
            Address[] flows = i.getFlows();
            for (Address f : flows) {
                Function fn = getFunctionAt(f);
                if (fn != null) sb.append("   -> ").append(fn.getName());
            }
            println(sb.toString());
        }
    }
}
