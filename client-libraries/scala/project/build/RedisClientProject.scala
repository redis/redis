import sbt._

class RedisClientProject(info: ProjectInfo) extends DefaultProject(info) with AutoCompilerPlugins
{
  override def useDefaultConfigurations = true
  
  val scalatest = "org.scala-tools.testing" % "scalatest" % "0.9.5" % "test->default"
  val specs = "org.scala-tools.testing" % "specs" % "1.5.0"
  val mockito = "org.mockito" % "mockito-all" % "1.7"
  val junit = "junit" % "junit" % "4.5"
  val sxr = compilerPlugin("org.scala-tools.sxr" %% "sxr" % "0.2.1")
}
