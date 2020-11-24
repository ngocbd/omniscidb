package com.mapd.parser.extension.ddl;

import static org.junit.Assert.assertEquals;

import com.google.gson.JsonObject;
import com.omnisci.thrift.calciteserver.TPlanResult;

import org.junit.Test;

public class InterruptCommandTest extends DDLTest {
  public InterruptCommandTest() {
    resourceDirPath =
            InterruptCommandTest.class.getClassLoader().getResource("").getPath();
    jsonTestDir = "interruptcommands";
  }

  @Test
  public void killQuery() throws Exception {
    final JsonObject expectedJsonObject = getJsonFromFile("kill_query.json");
    final TPlanResult result = processDdlCommand("KILL QUERY '123-a1b2';");
    final JsonObject actualJsonObject =
            gson.fromJson(result.plan_result, JsonObject.class);
    assertEquals(expectedJsonObject, actualJsonObject);
  }
}
