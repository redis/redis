<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">
  <xsl:param name="funcsynopsis.style">ansi</xsl:param>
  <xsl:param name="function.parens" select="0"/>
  <xsl:template match="function">
    <xsl:call-template name="inline.monoseq"/>
  </xsl:template>
  <xsl:template match="mallctl">
    <quote><xsl:call-template name="inline.monoseq"/></quote>
  </xsl:template>
</xsl:stylesheet>
