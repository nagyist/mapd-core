package com.mapd.parser.extension.ddl;

import static java.util.Objects.requireNonNull;

import com.google.gson.annotations.Expose;

import org.apache.calcite.sql.SqlDdl;
import org.apache.calcite.sql.SqlKind;
import org.apache.calcite.sql.SqlNode;
import org.apache.calcite.sql.SqlNodeList;
import org.apache.calcite.sql.SqlOperator;
import org.apache.calcite.sql.SqlSpecialOperator;
import org.apache.calcite.sql.parser.SqlParserPos;
import org.apache.calcite.util.EscapedStringJsonBuilder;

import java.util.List;
import java.util.Map;

public class SqlRevokePrivilege extends SqlDdl {
  private static final SqlOperator OPERATOR =
          new SqlSpecialOperator("REVOKE_PRIVILEGE", SqlKind.OTHER_DDL);

  @Expose
  private SqlNodeList privileges;
  @Expose
  private String type;
  @Expose
  private String target;
  @Expose
  private SqlNodeList grantees;

  public SqlRevokePrivilege(SqlParserPos pos,
          SqlNodeList privileges,
          String type,
          String target,
          SqlNodeList grantees) {
    super(OPERATOR, pos);
    requireNonNull(privileges);
    this.privileges = privileges;
    this.type = type;
    this.target = target;
    this.grantees = grantees;
  }

  @Override
  public List<SqlNode> getOperandList() {
    return null;
  }

  @Override
  public String toString() {
    EscapedStringJsonBuilder jsonBuilder = new EscapedStringJsonBuilder();
    Map<String, Object> map = jsonBuilder.map();

    if (this.privileges != null) {
      List<Object> privilege_list = jsonBuilder.list();
      for (SqlNode node : this.privileges) {
        assert node instanceof SqlPrivilege;
        SqlPrivilege privilege = (SqlPrivilege) node;
        privilege_list.add(privilege.toJson(jsonBuilder));
      }
      map.put("privileges", privilege_list);
    }

    map.put("type", this.type);
    map.put("target", this.target);

    if (this.grantees != null) {
      List<Object> grantee_list = jsonBuilder.list();
      for (SqlNode grantee : this.grantees) {
        grantee_list.add(grantee.toString());
      }
      map.put("grantees", grantee_list);
    }

    map.put("command", "REVOKE_PRIVILEGE");
    Map<String, Object> payload = jsonBuilder.map();
    payload.put("payload", map);
    return jsonBuilder.toJsonString(payload);
  }
}
