if(NOT DEFINED AMALGAM_LAUNCHER OR NOT DEFINED AMALGAM_JAVAC OR
   NOT DEFINED AMALGAM_JAVA OR NOT DEFINED AMALGAM_JAR OR
   NOT DEFINED AMALGAM_WORK)
    message(FATAL_ERROR "Server transport test inputs are incomplete")
endif()

file(REMOVE_RECURSE "${AMALGAM_WORK}")
file(MAKE_DIRECTORY "${AMALGAM_WORK}/src" "${AMALGAM_WORK}/classes"
                    "${AMALGAM_WORK}/probe/libraries/net/minecraftforge/forge/1.20.1-47.4.23")
file(WRITE "${AMALGAM_WORK}/src/EchoServer.java" [=[
import java.io.BufferedReader;
import java.io.InputStreamReader;

public final class EchoServer {
    public static void main(String[] args) throws Exception {
        System.out.println("[INFO] READY");
        BufferedReader input = new BufferedReader(new InputStreamReader(System.in));
        String line;
        while ((line = input.readLine()) != null) {
            System.out.println("[INFO] ECHO:" + line);
            if (line.equals("stop")) break;
        }
    }
}
]=])

execute_process(
    COMMAND "${AMALGAM_JAVAC}" -encoding UTF-8 -d "${AMALGAM_WORK}/classes"
            "${AMALGAM_WORK}/src/EchoServer.java"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "Echo server compilation failed: ${compile_output}${compile_error}")
endif()

execute_process(
    COMMAND "${AMALGAM_JAR}" --create --file "${AMALGAM_WORK}/probe/server.jar"
            --main-class EchoServer -C "${AMALGAM_WORK}/classes" EchoServer.class
    RESULT_VARIABLE jar_result
    OUTPUT_VARIABLE jar_output
    ERROR_VARIABLE jar_error)
if(NOT jar_result EQUAL 0)
    message(FATAL_ERROR "Echo server jar creation failed: ${jar_output}${jar_error}")
endif()
file(WRITE "${AMALGAM_WORK}/probe/server.properties"
     "server-name=probe\nserver-port=25565\nmax-players=1\n")
# A current Forge server starts Java through a response file. Keep this under
# AMALGAM_WORK (which is inside "Default Project") so the test proves that the
# @-prefixed path reaches Java as one argument rather than splitting at spaces.
file(WRITE "${AMALGAM_WORK}/probe/libraries/net/minecraftforge/forge/1.20.1-47.4.23/win_args.txt"
     "-cp\nserver.jar\nEchoServer\n")

execute_process(
    COMMAND "${AMALGAM_LAUNCHER}" --server-transport-probe
            "${AMALGAM_WORK}" "${AMALGAM_JAVA}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_error
    TIMEOUT 20)
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "Server transport probe failed: ${probe_output}${probe_error}")
endif()
if(NOT probe_output MATCHES "server transport: READY")
    message(FATAL_ERROR "Server transport probe did not report readiness: ${probe_output}")
endif()
# Local servers must start with managed Java from the same root --check-java
# reports, and never from a per-user directory the launcher does not create.
if(NOT probe_output MATCHES "server java root: .*runtimes[\\/]java")
    message(FATAL_ERROR "Server transport probe did not report the managed Java root: ${probe_output}")
endif()
if(probe_output MATCHES "server java root: .*AppData")
    message(FATAL_ERROR "Server Java root is not a directory the launcher creates: ${probe_output}")
endif()

file(REMOVE_RECURSE "${AMALGAM_WORK}")
message(STATUS "${probe_output}")
