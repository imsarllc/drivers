def promotionConfig = [
    // Mandatory parameters
    'targetRepo'         : 'fpga-deb-release',

    // Optional parameters

    // The build name and build number to promote. If not specified, the Jenkins job's build name and build number are used
    'buildName'          : 'gpl_kernel_drivers',
    'buildNumber'        : env.BUILD_NUMBER,
    // Comment and Status to be displayed in the Build History tab in Artifactory
    'comment'            : 'this is the promotion comment',
    'status'             : 'Released',
    // Specifies the source repository for build artifacts.
    'sourceRepo'         : 'fpga-deb-nightly',
    // Indicates whether to promote the build dependencies, in addition to the artifacts. False by default
    //'includeDependencies': true,
    // Indicates whether to copy the files. Move is the default
    'copy'               : true,
    // Indicates whether to fail the promotion process in case of failing to move or copy one of the files. False by default.
    'failFast'           : true
]

node('kernel')
{
  stage('Checkout')
  {
    cleanWs()
    checkout scm
  }
  stage('Build 2016.4')
  {
    sh('''\
      #!/bin/bash
      source /etc/profile.d/rvm.sh; 
      type rvm | head -n 1; 
      rvm use
      make 2016.4'''.stripIndent())
  }
  stage('Build 2019.2')
  {
    sh('''\
      #!/bin/bash
      source /etc/profile.d/rvm.sh;
      type rvm | head -n 1;
      rvm use
      export CROSS_COMPILE?=/fpga_tools/Xilinx/Vitis/2019.2/gnu/aarch32/lin/gcc-arm-linux-gnueabi/bin/arm-linux-gnueabihf-
      make 2019.2'''.stripIndent())
  }
  stage('Archive')
  {
    archiveArtifacts artifacts: 'build/**', fingerprint: true
  }
  if(env.BRANCH_NAME == "master")
  {
    stage('Upload')
    {
      def uploadSpec = """{
        "files": [
          {
            "pattern": "*.deb",
            "target": "fpga-deb-nightly/pool/grizzly-kernel/",
            "props": "deb.distribution=xenial;deb.distribution=bionic;deb.component=contrib;deb.architecture=armhf"
          }
        ]
      }"""
      def server = Artifactory.server 'artifactory'
      def buildInfo = Artifactory.newBuildInfo()
      buildInfo.env.capture = true
      buildInfo.env.collect()
      buildInfo.name = 'gpl_kernel_drivers'
      buildInfo.retention maxBuilds: 10, deleteBuildArtifacts: true, async: true
      server.upload spec: uploadSpec, buildInfo: buildInfo
      server.publishBuildInfo buildInfo

      Artifactory.addInteractivePromotion server: server, promotionConfig: promotionConfig, displayName: "Promote me please"
    }
  }
}
