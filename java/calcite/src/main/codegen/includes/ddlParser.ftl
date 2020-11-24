<#--
 Copyright 2020 OmniSci, Inc.

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
-->

SqlNodeList TableElementList() :
{
    final Span s;
    final List<SqlNode> list = new ArrayList<SqlNode>();
}
{
    <LPAREN> { s = span(); }
    TableElement(list)
    (
        <COMMA> TableElement(list)
    )*
    <RPAREN> {
        return new SqlNodeList(list, s.end(this));
    }
}

// Parse an optional data type encoding, default is NONE.
Pair<OmniSciEncoding, Integer> OmniSciEncodingOpt() :
{
    OmniSciEncoding encoding;
    Integer size = 0;
}
{
    <ENCODING>
    (
        <NONE> { encoding = OmniSciEncoding.NONE; }
    |
        <FIXED> { encoding = OmniSciEncoding.FIXED; }
        [ <LPAREN> size = IntLiteral() <RPAREN> ]
    |
        <DAYS> { encoding = OmniSciEncoding.DAYS; }
        [ <LPAREN> size = IntLiteral() <RPAREN> ]
    | 
        <DICT> { encoding = OmniSciEncoding.DICT; }
        [ <LPAREN> size = IntLiteral() <RPAREN> ]
    | 
        <COMPRESSED> { encoding = OmniSciEncoding.COMPRESSED; }
        [ <LPAREN> size = IntLiteral() <RPAREN> ]
    )
    { return new Pair(encoding, size); }
}

// Parse sql type name that allow arrays
SqlTypeNameSpec OmniSciArrayTypeName(Span s) :
{
    final SqlTypeName sqlTypeName;
    boolean isText = false;
    Integer size = -1;
}
{
    (
        <BOOLEAN> { sqlTypeName = SqlTypeName.BOOLEAN; }
    |
        ( <INTEGER> | <INT> ) { sqlTypeName = SqlTypeName.INTEGER; }
    |
        <TINYINT> { sqlTypeName = SqlTypeName.TINYINT; }
    |
        <SMALLINT> { sqlTypeName = SqlTypeName.SMALLINT; }
    |
        <BIGINT> { sqlTypeName = SqlTypeName.BIGINT; }
    |
        <DOUBLE> { sqlTypeName = SqlTypeName.DOUBLE; }
    |
        <FLOAT> { sqlTypeName = SqlTypeName.FLOAT; }
    |
        <TEXT> { 
            isText = true;
            sqlTypeName = SqlTypeName.VARCHAR; // TODO: consider overloading / adding TEXT as base type
        }
    |
        <DATE> { sqlTypeName = SqlTypeName.DATE; }
    |
        <TIME> { sqlTypeName = SqlTypeName.TIME; }
    )
     <LBRACKET>
    (
        size = UnsignedIntLiteral()
        <RBRACKET>
    |
        <RBRACKET>
    )
    {
        return new OmniSciTypeNameSpec(sqlTypeName, isText, size, s.end(this));
    }
}

// Parse DECIMAL that allows arrays
SqlTypeNameSpec OmniSciDecimalArrayTypeName(Span s) :
{
    final SqlTypeName sqlTypeName;
    Integer size = -1;
    Integer precision = -1;
    Integer scale = -1;
}
{
    (<DECIMAL> | <DEC> | <NUMERIC>) { sqlTypeName = SqlTypeName.DECIMAL; }
    (
        <LPAREN>
        precision = UnsignedIntLiteral()
        [
            <COMMA>
            scale = UnsignedIntLiteral()
        ]
        <RPAREN>
    )
     <LBRACKET>
    (
        size = UnsignedIntLiteral()
        <RBRACKET>
    |
        <RBRACKET>
    )
    {
        return new OmniSciTypeNameSpec(sqlTypeName, false, size, precision, scale, s.end(this));
    }
}

// Parse sql TIMESTAMP that allow arrays 
SqlTypeNameSpec OmniSciTimestampArrayTypeName(Span s) :
{
    final SqlTypeName sqlTypeName;
    Integer size = -1;
    Integer precision = -1;
    final Integer scale = -1;
}
{
    <TIMESTAMP> { sqlTypeName = SqlTypeName.TIMESTAMP; }
    [
        <LPAREN>
        precision = UnsignedIntLiteral()
        <RPAREN>
    ]
     <LBRACKET>
    (
        size = UnsignedIntLiteral()
        <RBRACKET>
    |
        <RBRACKET>
    )
    {
        return new OmniSciTypeNameSpec(sqlTypeName, false, size, precision, scale, s.end(this));
    }
}

// Parse sql TEXT type 
SqlTypeNameSpec OmniSciTextTypeName(Span s) :
{
    final SqlTypeName sqlTypeName;
    boolean isText = false;
}
{
    
    <TEXT> { 
        isText = true;
        sqlTypeName = SqlTypeName.VARCHAR; // TODO: consider overloading / adding TEXT as base type
    }
    {
        return new OmniSciTypeNameSpec(sqlTypeName, isText, s.end(this));
    }
}

OmniSciGeo OmniSciGeoType() :
{
    final OmniSciGeo geoType;
}
{
    (
        <POINT> { geoType = OmniSciGeo.POINT; }
    |   
        <LINESTRING> { geoType = OmniSciGeo.LINESTRING; }
    | 
        <POLYGON> { geoType = OmniSciGeo.POLYGON; }
    |
        <MULTIPOLYGON> { geoType = OmniSciGeo.MULTIPOLYGON; }
    )
    {
        return geoType;
    }
}

// Parse sql type name for geospatial data 
SqlTypeNameSpec OmniSciGeospatialTypeName(Span s) :
{
    final OmniSciGeo geoType;
    boolean isGeography = false;
    Integer coordinateSystem = 0;
    Pair<OmniSciEncoding, Integer> encoding = null;
}
{
    (
        geoType = OmniSciGeoType()
        |
        (
            <GEOMETRY> { 
                isGeography = false;
            }
        |
            <GEOGRAPHY> { 
                isGeography = true;
            }
        )
        <LPAREN> geoType = OmniSciGeoType() [ <COMMA> coordinateSystem = IntLiteral() ] <RPAREN>
        [ encoding = OmniSciEncodingOpt() ]
    )
    {
        return new OmniSciGeoTypeNameSpec(geoType, coordinateSystem, isGeography, encoding, s.end(this));
    }
}

// Some SQL type names need special handling due to the fact that they have
// spaces in them but are not quoted.
SqlTypeNameSpec OmniSciTypeName() :
{
    final SqlTypeNameSpec typeNameSpec;
    final SqlIdentifier typeName;
    final Span s = Span.of();
}
{
    (
<#-- additional types are included here -->
<#-- put custom data types in front of Calcite core data types -->
        LOOKAHEAD(2)
        typeNameSpec = OmniSciArrayTypeName(s)
    |
        LOOKAHEAD(5)
        typeNameSpec = OmniSciTimestampArrayTypeName(s)
    |
        LOOKAHEAD(7)
        typeNameSpec = OmniSciDecimalArrayTypeName(s)
    |
        LOOKAHEAD(2)
        typeNameSpec = OmniSciTextTypeName(s)
    |
        LOOKAHEAD(2)
        typeNameSpec = OmniSciGeospatialTypeName(s)
    |
<#list parser.dataTypeParserMethods as method>
        LOOKAHEAD(2)
        typeNameSpec = ${method}
    |
</#list>
        LOOKAHEAD(2)
        typeNameSpec = SqlTypeName(s)
    |
        typeNameSpec = RowTypeName()
    |
        typeName = CompoundIdentifier() {
            typeNameSpec = new SqlUserDefinedTypeNameSpec(typeName, s.end(this));
        }
    )
    {
        return typeNameSpec;
    }
}


// Type name with optional scale and precision.
OmniSciSqlDataTypeSpec OmniSciDataType() :
{
    SqlTypeNameSpec typeName;
    final Span s;
}
{
    typeName = OmniSciTypeName() {
        s = span();
    }
    (
        typeName = CollectionsTypeName(typeName)
    )*
    {
        return new OmniSciSqlDataTypeSpec(
            typeName,
            s.end(this));
    }
}

void OmniSciShardKeyOpt(List<SqlNode> list) : 
{
    final Span s;
    final SqlIdentifier name;
}
{
    <SHARD> { s = span(); } <KEY> 
    <LPAREN> name = SimpleIdentifier() {
        list.add(SqlDdlNodes.shard(s.end(this), name));
    }
    <RPAREN> 
}

SqlIdentifier OmniSciSharedDictReferences() :
{
    final Span s = Span.of();
    SqlIdentifier name;
    final SqlIdentifier name2;
}
{
    (
        LOOKAHEAD(2)
        name = SimpleIdentifier() 
        <LPAREN> name2 = SimpleIdentifier() <RPAREN> {
            name = name.add(1, name2.getSimple(), s.end(this));
        }
    |
        name = SimpleIdentifier()
    )
    {
        return name;
    }
}

void OmniSciSharedDictOpt(List<SqlNode> list) :
{
    final Span s;
    final SqlIdentifier columnName;
    final SqlIdentifier referencesColumn;
}
{
    <SHARED> { s = span(); } <DICTIONARY>
    <LPAREN> columnName = SimpleIdentifier() <RPAREN>
    <REFERENCES> 
    referencesColumn = OmniSciSharedDictReferences()
    {
        list.add(SqlDdlNodes.sharedDict(s.end(this), columnName, referencesColumn));
    }
}

void TableElement(List<SqlNode> list) :
{
    final SqlIdentifier id;
    final OmniSciSqlDataTypeSpec type;
    final boolean nullable;
    Pair<OmniSciEncoding, Integer> encoding = null;
    final SqlNode e;
    final SqlNode constraint;
    SqlIdentifier name = null;
    final SqlNodeList columnList;
    final Span s = Span.of();
    final ColumnStrategy strategy;
}
{
    (
        LOOKAHEAD(3)
        OmniSciShardKeyOpt(list)
    |
        OmniSciSharedDictOpt(list)
    |
        (
            id = SimpleIdentifier()
            (
                type = OmniSciDataType()
                nullable = NullableOptDefaultTrue()
                [ encoding = OmniSciEncodingOpt() ]
                (
                    [ <GENERATED> <ALWAYS> ] <AS> <LPAREN>
                    e = Expression(ExprContext.ACCEPT_SUB_QUERY) <RPAREN>
                    (
                        <VIRTUAL> { strategy = ColumnStrategy.VIRTUAL; }
                    |
                        <STORED> { strategy = ColumnStrategy.STORED; }
                    |
                        { strategy = ColumnStrategy.VIRTUAL; }
                    )
                |
                    <DEFAULT_> e = Expression(ExprContext.ACCEPT_SUB_QUERY) {
                        strategy = ColumnStrategy.DEFAULT;
                    }
                |
                    {
                        e = null;
                        strategy = nullable ? ColumnStrategy.NULLABLE
                            : ColumnStrategy.NOT_NULLABLE;
                    }
                )
                {
                    list.add(
                        SqlDdlNodes.column(s.add(id).end(this), id,
                            type.withEncoding(encoding).withNullable(nullable), e, strategy));
                }
            |
                { list.add(id); }
            )
        )
    )
}

/*
 * Parse an option key/value pair. The value is expected to be a non-interval literal. 
 *
 * <option> = <value>
 */
Pair<String, SqlNode> KVOption() :
{
    final SqlIdentifier withOption;
    final String withOptionString;
    final SqlNode withValue;
}
{
    (
      // Special rule required to handle "escape" option, since ESCAPE is a keyword
      <ESCAPE>
      {
        withOptionString = "escape";
      }
    |
      withOption = CompoundIdentifier()
      {
        withOptionString = withOption.toString();
      }
    )
    <EQ>
    withValue = Literal()
    { return new Pair<String, SqlNode>(withOptionString, withValue); }
}

/*
 * Parse one or more WITH clause key-value pair options
 *
 * WITH ( <option> = <value> [, ... ] )
 */
OmniSciOptionsMap WithOptionsOpt() :
{
    OmniSciOptionsMap optionMap = new OmniSciOptionsMap();
    Pair<String, SqlNode> optionPair = null;
}
{
  <WITH> <LPAREN>
  optionPair = KVOption()
  { OmniSciOptionsMap.add(optionMap, optionPair.getKey(), optionPair.getValue()); }
  (
    <COMMA>
    optionPair = KVOption()
    { OmniSciOptionsMap.add(optionMap, optionPair.getKey(), optionPair.getValue()); }
  )*
  <RPAREN>
  { return optionMap; }
}

SqlCreate SqlCreateTable(Span s, boolean replace) :
{
    final boolean ifNotExists;
    final SqlIdentifier id;
    SqlNodeList tableElementList = null;
    OmniSciOptionsMap withOptions = null;
    SqlNode query = null;
}
{
    <TABLE> ifNotExists = IfNotExistsOpt() id = CompoundIdentifier()
    [ tableElementList = TableElementList() ]
    [ <AS> query = OrderedQueryOrExpr(ExprContext.ACCEPT_QUERY) ]
    [ withOptions = WithOptionsOpt() ]
    {
        return SqlDdlNodes.createTable(s.end(this), replace, ifNotExists, id,
            tableElementList, withOptions, query);
    }
}

SqlCreate SqlCreateView(Span s, boolean replace) :
{
    final boolean ifNotExists;
    final SqlIdentifier id;
    SqlNodeList columnList = null;
    final SqlNode query;
}
{
    <VIEW> ifNotExists = IfNotExistsOpt() id = CompoundIdentifier()
    [ columnList = ParenthesizedSimpleIdentifierList() ]
    <AS> query = OrderedQueryOrExpr(ExprContext.ACCEPT_QUERY) {
        if (columnList != null && columnList.size() > 0) {
            throw new ParseException("Column list aliases in views are not yet supported.");
        }
        return SqlDdlNodes.createView(s.end(this), replace, ifNotExists, id, columnList,
            query);
    }
}
