/* Decompiles nsysuhs.rpl's transfer entry points.
 *
 * The fault is on this side of the boundary: IOSU logs the three ioctls
 * it receives and has no trace of the one ioctlv, so the bulk request is
 * not reaching it. This is the code that builds it.
 */
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompPpc extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[][] targets = {
            { "02001-5e4", "UhsSubmitBulkRequest" },
            { "0200-0dd0", "UhsSubmitControlRequest" },
        };
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        String[] addrs = { "020015e4", "02000dd0", "02000c34" };
        String[] names = { "UhsSubmitBulkRequest", "UhsSubmitControlRequest",
                           "UhsAdministerEndpoint" };
        for (int i = 0; i < addrs.length; i++) {
            Address a = currentProgram.getAddressFactory().getAddress("0x" + addrs[i]);
            Function f = getFunctionAt(a);
            if (f == null) {
                f = createFunction(a, names[i]);
            }
            println("======== " + names[i] + " @ " + addrs[i] + " ========");
            if (f == null) {
                println("(could not make a function there)");
                continue;
            }
            DecompileResults r = decomp.decompileFunction(f, 120, monitor);
            println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC()
                                                        : "(failed)");
        }
        decomp.dispose();
    }
}
