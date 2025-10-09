#!/usr/bin/env node
/**
 * Redis Table Module - Client Compatibility Test Suite (Node.js)
 * Tests that special characters in arguments work correctly with node-redis
 */

const redis = require('redis');

async function testNodeClient() {
    console.log('='.repeat(60));
    console.log('Redis Table Module - Node.js Client Compatibility Test');
    console.log('='.repeat(60));
    
    const client = redis.createClient();
    
    try {
        await client.connect();
        console.log('✓ Connected to Redis');
    } catch (e) {
        console.error(`✗ Failed to connect to Redis: ${e.message}`);
        return false;
    }
    
    const testNamespace = 'test_js';
    const testTable = `${testNamespace}.compatibility`;
    
    try {
        // Cleanup
        try {
            await client.sendCommand(['TABLE.DROP', testTable, 'FORCE']);
        } catch (e) {
            // Ignore if doesn't exist
        }
        
        console.log('\n' + '='.repeat(60));
        console.log('Test 1: Special Characters in Table Names (dots)');
        console.log('='.repeat(60));
        
        // Test 1: Create namespace
        let result = await client.sendCommand(['TABLE.NAMESPACE.CREATE', testNamespace]);
        if (result !== 'OK') throw new Error(`Expected OK, got ${result}`);
        console.log(`✓ PASS: Created namespace with name: ${testNamespace}`);
        
        // Test 2: Create table with dots in name
        result = await client.sendCommand([
            'TABLE.SCHEMA.CREATE',
            testTable,
            'NAME:string:true',
            'AGE:integer:false',
            'EMAIL:string:true'
        ]);
        if (result !== 'OK') throw new Error(`Expected OK, got ${result}`);
        console.log(`✓ PASS: Created table with dot in name: ${testTable}`);
        
        console.log('\n' + '='.repeat(60));
        console.log('Test 2: Special Characters in Column Definitions (colons)');
        console.log('='.repeat(60));
        
        // Verify schema contains colons
        const schema = await client.sendCommand(['TABLE.SCHEMA.VIEW', testTable]);
        const schemaStr = JSON.stringify(schema);
        if (!schemaStr.includes('NAME')) throw new Error('NAME column not found');
        if (!schemaStr.includes('string')) throw new Error('string type not found');
        console.log('✓ PASS: Column definitions with colons parsed correctly');
        console.log(`  Schema: ${schemaStr.substring(0, 100)}...`);
        
        console.log('\n' + '='.repeat(60));
        console.log('Test 3: Special Characters in Insert (equals signs)');
        console.log('='.repeat(60));
        
        // Test 3: Insert with equals signs
        const insertResult = await client.sendCommand([
            'TABLE.INSERT',
            testTable,
            'NAME=John Doe',
            'AGE=30',
            'EMAIL=john@example.com'
        ]);
        if (!insertResult) throw new Error('Insert failed');
        console.log('✓ PASS: Inserted data with equals signs: NAME=John Doe');
        console.log(`  Returned ID: ${insertResult}`);
        
        console.log('\n' + '='.repeat(60));
        console.log('Test 4: Special Characters in WHERE (operators)');
        console.log('='.repeat(60));
        
        // Test 4: Query with operators
        let results = await client.sendCommand([
            'TABLE.SELECT',
            testTable,
            'WHERE',
            'AGE>25'
        ]);
        if (!results || results.length === 0) throw new Error('No results found');
        console.log('✓ PASS: Query with > operator worked');
        console.log(`  Results count: ${results.length}`);
        
        // Test 5: Query with = operator
        results = await client.sendCommand([
            'TABLE.SELECT',
            testTable,
            'WHERE',
            'NAME=John Doe'
        ]);
        if (!results || results.length === 0) throw new Error('No results with equality');
        console.log('✓ PASS: Query with = operator worked');
        
        console.log('\n' + '='.repeat(60));
        console.log('Test 5: Special Characters in Complex Queries (AND/OR)');
        console.log('='.repeat(60));
        
        // Insert more data
        await client.sendCommand([
            'TABLE.INSERT',
            testTable,
            'NAME=Jane Smith',
            'AGE=28',
            'EMAIL=jane@example.com'
        ]);
        
        // Test 6: Complex query with AND
        results = await client.sendCommand([
            'TABLE.SELECT',
            testTable,
            'WHERE',
            'AGE>25',
            'AND',
            'NAME=John Doe'
        ]);
        if (!results || results.length === 0) throw new Error('AND query failed');
        console.log('✓ PASS: Complex query with AND operator worked');
        
        console.log('\n' + '='.repeat(60));
        console.log('Test 6: Special Characters in UPDATE (equals in SET)');
        console.log('='.repeat(60));
        
        // Test 7: Update with equals
        result = await client.sendCommand([
            'TABLE.UPDATE',
            testTable,
            'WHERE',
            'NAME=John Doe',
            'SET',
            'AGE=31'
        ]);
        if (!result) throw new Error('Update failed');
        console.log('✓ PASS: Updated with equals in SET clause');
        
        // Verify update
        results = await client.sendCommand([
            'TABLE.SELECT',
            testTable,
            'WHERE',
            'NAME=John Doe'
        ]);
        const resultStr = JSON.stringify(results);
        if (!resultStr.includes('31')) throw new Error('Update not reflected');
        console.log('✓ PASS: Update verified in query results');
        
        console.log('\n' + '='.repeat(60));
        console.log('Test 7: Spaces in Values');
        console.log('='.repeat(60));
        
        // Test 8: Spaces in values
        await client.sendCommand([
            'TABLE.INSERT',
            testTable,
            'NAME=Bob Johnson Jr.',
            'AGE=45',
            'EMAIL=bob.jr@example.com'
        ]);
        
        results = await client.sendCommand([
            'TABLE.SELECT',
            testTable,
            'WHERE',
            'NAME=Bob Johnson Jr.'
        ]);
        if (!results || results.length === 0) throw new Error('Spaces in values failed');
        console.log('✓ PASS: Values with spaces handled correctly');
        console.log("  Name: 'Bob Johnson Jr.'");
        
        console.log('\n' + '='.repeat(60));
        console.log('Test 8: Special Characters in Email (@ symbol)');
        console.log('='.repeat(60));
        
        results = await client.sendCommand([
            'TABLE.SELECT',
            testTable,
            'WHERE',
            'EMAIL=john@example.com'
        ]);
        if (!results || results.length === 0) throw new Error('@ symbol in value failed');
        console.log('✓ PASS: Email with @ symbol handled correctly');
        
        // Cleanup
        await client.sendCommand(['TABLE.DROP', testTable, 'FORCE']);
        console.log('\n✓ PASS: Cleanup successful');
        
        console.log('\n' + '='.repeat(60));
        console.log('ALL TESTS PASSED - Node.js client is fully compatible!');
        console.log('='.repeat(60));
        
        return true;
        
    } catch (error) {
        console.error(`\n✗ FAIL: ${error.message}`);
        console.error(error.stack);
        return false;
    } finally {
        // Final cleanup
        try {
            await client.sendCommand(['TABLE.DROP', testTable, 'FORCE']);
        } catch (e) {
            // Ignore
        }
        await client.disconnect();
    }
}

// Run tests
testNodeClient().then(success => {
    process.exit(success ? 0 : 1);
}).catch(error => {
    console.error('Fatal error:', error);
    process.exit(1);
});
