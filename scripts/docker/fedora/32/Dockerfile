FROM fedora:32

RUN dnf upgrade -y && dnf install -y \
        augeas-devel \
        bison \
        bison-devel \
        cmake \
        curl \
        dbus-devel \
        diffutils \
        dnf-plugins-core \
        file \
        findutils \
        flex \
        flex-devel \
        gcc-c++ \
        git \
        glib2 \
        gpgme-devel \
        java-11-openjdk-devel \
        jna \
        libasan \
        libcurl-devel \
        libev-devel \
        libgcrypt-devel \
        libgit2-devel \
        libmarkdown-devel \
        libubsan \
        libuv-devel \
        libxml2-devel \
        llvm \
        lua-devel \
        make \
        ninja-build \
        openssl-devel \
        procps-ng \
        python3-devel \
        qt5-devel \
        ruby-devel \
        rubygem-test-unit \
        swig \
        unzip \
        valgrind \
        wget \
        which \
        xerces-c-devel \
        yajl-devel \
        yaml-cpp-devel \
        zlib-devel \
    && dnf debuginfo-install -y \
        bison \
        flex \
        glib2 \
        glibc \
        gpgme \
        libgcrypt \
        ruby-libs \
    && dnf clean all -y

# Google Test
ENV GTEST_ROOT=/opt/gtest
ARG GTEST_VER=release-1.11.0
RUN mkdir -p ${GTEST_ROOT} \
    && cd /tmp \
    && curl -o gtest.tar.gz \
      -L https://github.com/google/googletest/archive/${GTEST_VER}.tar.gz \
    && tar -zxvf gtest.tar.gz --strip-components=1 -C ${GTEST_ROOT} \
    && rm gtest.tar.gz

# Create User:Group
# The id is important as jenkins docker agents use the same id that is running
# on the slaves to execute containers
ARG JENKINS_GROUPID
RUN groupadd \
    -g ${JENKINS_GROUPID} \
    -f \
    jenkins

ARG JENKINS_USERID
RUN useradd \
    --create-home \
    --uid ${JENKINS_USERID} \
    --gid ${JENKINS_GROUPID} \
    --shell "/bin/bash" \
    jenkins

# download and install gradle
RUN cd /tmp && wget https://services.gradle.org/distributions/gradle-7.1.1-bin.zip && unzip gradle-7.1.1-bin.zip && rm gradle-7.1.1-bin.zip && mv gradle-7.1.1 /opt/gradle
ENV PATH "${PATH}:/opt/gradle/bin"
ENV JAVA_HOME=/etc/alternatives/jre
# Pipe in 1 as Java 11 is first in the list
RUN echo 1 | alternatives --config java && alternatives --auto javac

USER ${JENKINS_USERID}

# extend PATH for user jenkins to include gradle
RUN echo "export PATH=${PATH}:/opt/gradle/bin" >> /home/jenkins/.bashrc

# Set git config
RUN git config --global user.email 'Jenkins <autobuilder@libelektra.org>' \
    && git config --global user.name 'Jenkins'
