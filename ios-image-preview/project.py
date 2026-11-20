"""Generate a minimal, checkout-independent App/XCTest project."""
import json

def project(directory):
    objects = {}
    def make(isa, **fields):
        ident = f'{len(objects)+1:024X}'
        objects[ident] = dict(isa=isa, **fields)
        return ident
    def configs(settings):
        debug = make('XCBuildConfiguration', name='Debug', buildSettings=settings)
        return make('XCConfigurationList', buildConfigurations=[debug],
                    defaultConfigurationIsVisible=0, defaultConfigurationName='Debug')
    sources, products = [], []
    defaults = dict(GENERATE_INFOPLIST_FILE='YES', CLANG_ENABLE_OBJC_ARC='YES',
                    CLANG_ENABLE_MODULES='YES', CLANG_CXX_LANGUAGE_STANDARD='c++17',
                    IPHONEOS_DEPLOYMENT_TARGET='15.0', SDKROOT='iphoneos',
                    SUPPORTED_PLATFORMS='iphoneos iphonesimulator', ARCHS='arm64',
                    TARGETED_DEVICE_FAMILY='1,2', CODE_SIGNING_ALLOWED='NO')
    def target(name, kind, files, settings):
        refs = [make('PBXFileReference', path=file, sourceTree='<group>', lastKnownFileType=typ)
                for file, typ in files]
        sources.extend(refs)
        def phase(type_, entries):
            return make(type_, files=entries, buildActionMask=2147483647,
                        runOnlyForDeploymentPostprocessing=0)
        source_phase = phase('PBXSourcesBuildPhase', [make('PBXBuildFile', fileRef=x) for x in refs])
        link = []
        if kind == 'application':
            archive = make('PBXFileReference', path='Installed/lib/libmadmedia_ios_image_preview.a',
                           sourceTree='SOURCE_ROOT', lastKnownFileType='archive.ar')
            sources.append(archive)
            link.append(make('PBXBuildFile', fileRef=archive))
        frameworks = phase('PBXFrameworksBuildPhase', link)
        path, ptype = (name+'.app','wrapper.application') if kind=='application' else (name+'.xctest','wrapper.cfbundle')
        product = make('PBXFileReference', path=path, explicitFileType=ptype, sourceTree='BUILT_PRODUCTS_DIR')
        products.append(product)
        return make('PBXNativeTarget', name=name, productName=name, productReference=product,
                    productType='com.apple.product-type.'+kind, buildPhases=[source_phase,frameworks],
                    buildRules=[], dependencies=[], buildConfigurationList=configs(
                        dict(defaults, PRODUCT_NAME=name, PRODUCT_BUNDLE_IDENTIFIER='org.madmedia.preview.'+name,
                             **settings)))
    app = target('PreviewApp','application',[('App.mm','sourcecode.cpp.objcpp')],
                 dict(OTHER_LDFLAGS=['-framework','Foundation','-framework','UIKit','-framework','CoreFoundation','-framework','ImageIO','-framework','CoreGraphics'],
                      HEADER_SEARCH_PATHS=['$(PROJECT_DIR)/Installed/include'], ENABLE_DEBUG_DYLIB='NO', DEAD_CODE_STRIPPING='NO', INFOPLIST_KEY_UILaunchScreen_Generation='YES'))
    tests = target('PreviewTests','bundle.unit-test',[('Tests.mm','sourcecode.cpp.objcpp')],
                   dict(TEST_HOST='$(BUILT_PRODUCTS_DIR)/PreviewApp.app/PreviewApp', BUNDLE_LOADER='$(TEST_HOST)',
                        OTHER_LDFLAGS=['-framework','Foundation','-framework','XCTest'],
                        FRAMEWORK_SEARCH_PATHS=['$(inherited)','$(PLATFORM_DIR)/Developer/Library/Frameworks']))
    pg = make('PBXGroup', children=products, name='Products', sourceTree='<group>')
    group = make('PBXGroup', children=sources+[pg], sourceTree='<group>')
    root = make('PBXProject', buildConfigurationList=configs({}), compatibilityVersion='Xcode 14.0',
                developmentRegion='en', hasScannedForEncodings=0, knownRegions=['en'],
                mainGroup=group, productRefGroup=pg, projectDirPath='', projectRoot='', targets=[app,tests])
    proxy = make('PBXContainerItemProxy', containerPortal=root, proxyType=1, remoteGlobalIDString=app, remoteInfo='PreviewApp')
    objects[tests]['dependencies'] = [make('PBXTargetDependency', target=app, targetProxy=proxy)]
    def encode(value):
        if isinstance(value,dict):
            return '{ '+''.join(json.dumps(k)+' = '+encode(v)+'; ' for k,v in value.items())+'}'
        if isinstance(value,list):
            return '('+','.join(encode(x) for x in value)+')'
        return json.dumps(str(value))
    folder = directory/'Preview.xcodeproj'
    schemes = folder/'xcshareddata/xcschemes'
    schemes.mkdir(parents=True)
    (folder/'project.pbxproj').write_text('// !$*UTF8*$!\n'+encode(
        dict(archiveVersion=1, classes={}, objectVersion=56, objects=objects, rootObject=root)))
    def entry(ident, name, ext):
        return (f'<BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{ident}" '
                f'BuildableName="{name}.{ext}" BlueprintName="{name}" ReferencedContainer="container:Preview.xcodeproj"/>')
    build = entry(app,'PreviewApp','app')
    test = entry(tests,'PreviewTests','xctest')
    (schemes/'Preview.xcscheme').write_text(
        '<Scheme version="1.3"><BuildAction><BuildActionEntries><BuildActionEntry '
        'buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" '
        f'buildForAnalyzing="YES">{build}</BuildActionEntry></BuildActionEntries></BuildAction>'
        '<TestAction buildConfiguration="Debug" shouldUseLaunchSchemeArgsEnv="YES"><Testables>'
        f'<TestableReference skipped="NO">{test}</TestableReference></Testables></TestAction></Scheme>')
